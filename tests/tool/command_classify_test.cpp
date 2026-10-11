// wfr-㊸ @① 测试：词面静态分类器（L2 静态门禁）
// 纪律（problems/03 §5）：每个用例都核对类别 + 可复现的依据文本，不靠"看起来对"；
// 只读=白名单内且无写参数；其余类别都在 fail-closed 的方向（宁可多拦，不可放行）。
#include <gtest/gtest.h>

#include <string_view>

#include "cppchain/tool/command_classify.hpp"

using namespace cppchain::tool;

namespace {

void expect_ro(std::string_view cmd) {
  EXPECT_EQ(classify_bash_command(cmd), CommandClass::ReadOnly) << "命令: " << cmd;
}
void expect_write(std::string_view cmd) {
  EXPECT_EQ(classify_bash_command(cmd), CommandClass::Write) << "命令: " << cmd;
}
void expect_risky(std::string_view cmd) {
  EXPECT_EQ(classify_bash_command(cmd), CommandClass::Risky) << "命令: " << cmd;
}
void expect_unknown(std::string_view cmd) {
  EXPECT_EQ(classify_bash_command(cmd), CommandClass::Unknown) << "命令: " << cmd;
}

}  // namespace

// ---- 只读白名单：词面可证纯只读 → 免审放行 ----

TEST(CommandClassify, readonly_primitives) {
  expect_ro("ls");
  expect_ro("ls -la");
  expect_ro("cat file.txt");
  expect_ro("echo hello");
  expect_ro("pwd");
  expect_ro("head -5 x");
  expect_ro("tail -20 x");
  expect_ro("grep foo file");
  expect_ro("which g++");
  expect_ro("diff a b");
  expect_ro("stat /tmp");
  expect_ro("du -sh .");
  expect_ro("date");
  expect_ro("printf '%s\\n' hi");
  expect_ro("basename /a/b");
  expect_ro("dirname /a/b/c");
  expect_ro("test -f x");
  expect_ro("[ -f x ]");
  expect_ro("seq 1 10");
  expect_ro("uname -a");
  expect_ro("whoami");
}

TEST(CommandClassify, readonly_composites_and_wrappers) {
  expect_ro("cat a | grep x | wc -l");      // 管道：每个子命令都只读
  expect_ro("cd /tmp && ls");               // 复合：cd + ls
  expect_ro("true || false");
  expect_ro("timeout 10 ls");               // wrapper: timeout <数字>
  expect_ro("timeout -s KILL 10 ls");
  expect_ro("nice -n 5 ls");
  expect_ro("command ls");                  // 透明 wrapper
  expect_ro("sh -c 'ls'");                  // 嵌套 shell：递归解析脚本本体
  expect_ro("env");                          // env 单独 = 列环境
  expect_ro("env | grep HOME");
  expect_ro("ls # 注释不影响");               // 注释
  expect_ro("find /tmp -name '*.cpp'");     // find 只读形式（无 -exec/-delete）
  expect_ro("sed -n 1p file");              // sed 无 -i → 只读流编辑
  expect_ro("ls *.txt");                    // cat/ls 的未引号 glob 仍是读
}

TEST(CommandClassify, readonly_git_forms) {
  expect_ro("git status");
  expect_ro("git diff");
  expect_ro("git diff --cached");
  expect_ro("git log --oneline -5");
  expect_ro("git show HEAD");
  expect_ro("git rev-parse HEAD");
  expect_ro("git ls-files");
  expect_ro("git grep foo");
  expect_ro("git branch");
  expect_ro("git branch -a");
  expect_ro("git tag");
  expect_ro("git config user.name");
  expect_ro("git config --list");
  expect_ro("git remote -v");
}

// ---- 写：普通文件系统副作用 → 需审批 ----

TEST(CommandClassify, write_forms) {
  expect_write("echo x > f");
  expect_write("echo x >> f");
  expect_write("ls > f");
  expect_write("mv a b");
  expect_write("cp a b");
  expect_write("mkdir d");
  expect_write("touch f");
  expect_write("rm x");              // 无 -f/-r 的 rm → 普通写
  expect_write("chmod +x f");
  expect_write("base64 f");
  expect_write("unzip a.zip");
  expect_write("ls | tee out.txt");  // tee 是写
  expect_write("sed -i s/x/y/ f");  // sed -i 就地写
  expect_write("sed -i.bak s/x/y/ f");  // -i<suffix> 同样就地写
  expect_write("git add .");
  expect_write("git commit -m x");
  expect_write("git push");
  expect_write("git fetch");
  expect_write("git branch v2");     // 创建分支 = 写
  expect_write("git tag v1");        // 打标签 = 写
  expect_write("env FOO=1 ls");      // env 变环境 = 写
  expect_write("grep foo *.txt");    // glob 敏感的只读命令 + 未引号通配符 → 升级
}

// ---- 危险签名：强制审批 ----

TEST(CommandClassify, risky_forms) {
  expect_risky("rm -rf /tmp/x");
  expect_risky("rm -rf *");
  expect_risky("sudo ls");
  expect_risky("su -");
  expect_risky("curl http://example.com");
  expect_risky("wget http://example.com/f");
  expect_risky("docker run ubuntu");
  expect_risky("podman ps");
  expect_risky("ssh host");
  expect_risky("scp a host:b");
  expect_risky("rsync -a /x host:/y");
  expect_risky("eval 'ls'");
  expect_risky("python3 script.py");
  expect_risky("node app.js");
  expect_risky("npm install");
  expect_risky("pip install x");
  expect_risky("cargo build");
  expect_risky("kill -9 1234");
  expect_risky("systemctl restart nginx");
  expect_risky("dd if=/dev/zero of=/dev/sda");
  expect_risky("mkfs.ext4 /dev/sdb");
  expect_risky("git clean -fd");
  expect_risky("git push -f");
  expect_risky("git reset --hard HEAD");
  expect_risky("find / -exec rm {} \\;");
  expect_risky("ls && rm -rf x");        // 复合取最坏
  expect_risky("echo ok; curl example.com");
  expect_risky("ls | sudo rm -rf /");    // 管道取最坏
  expect_risky("sh -c 'rm -rf /'");      // 嵌套里是危险命令
  expect_risky("timeout 5 rm -rf x");    // wrapper 剥离后仍是危险命令
}

// ---- 未知 / 无法分析：fail-closed ----

TEST(CommandClassify, unknown_fail_closed) {
  expect_unknown("unknowncmd");
  expect_unknown("foobar arg1");
  expect_unknown("alias foo=bar");
  expect_unknown("my_script.sh");
  expect_unknown("git frobnicate");       // 未识别 git 子命令
  expect_unknown("echo \"$(ls)\"");       // 命令替换 → 无法静态分析
  expect_unknown("echo `ls`");            // 反引号同理
  expect_unknown("");                     // 空命令
  expect_unknown("   ");
  expect_unknown("sh -c 'bogus'");        // 嵌套里是未知命令
}

// ---- detail 入口：why 可复现（给审计/报错用）----

TEST(CommandClassify, detail_why_is_human_readable) {
  auto ro = classify_bash_command_detail("cat f");
  EXPECT_EQ(ro.cls, CommandClass::ReadOnly);
  EXPECT_NE(ro.why.find("只读"), std::string::npos);

  auto fw = classify_bash_command_detail("touch f");
  EXPECT_EQ(fw.cls, CommandClass::Write);
  EXPECT_NE(fw.why.find("touch"), std::string::npos);

  auto rs = classify_bash_command_detail("sudo ls");
  EXPECT_EQ(rs.cls, CommandClass::Risky);
  EXPECT_NE(rs.why.find("sudo"), std::string::npos);

  auto uk = classify_bash_command_detail("nope-command");
  EXPECT_EQ(uk.cls, CommandClass::Unknown);
  EXPECT_NE(uk.why.find("未知命令"), std::string::npos);
}