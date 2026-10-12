// cppchain —— src/tool/command_classify.cpp
// 词面静态分类实现（wfr-㊸ @①）。见 include/cppchain/tool/command_classify.hpp 的纪律说明：
// 只做保守裁决，能证纯只读才放行；解析/评估不了的走 Unknown（fail-closed）。
// 规则表与参数级安全检查（glob / -i / -exec / git 子命令）是批① 的可接受广度，
// 更大的广度（环境感知路径判断、更多子命令）留给后续迭代，见 01-discuss/06 §5 L2。
#include "cppchain/tool/command_classify.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cppchain::tool {
namespace {

// ---- 小工具 ----

bool space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trim_view(std::string_view s) {
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

std::string ascii_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

template <std::size_t N>
bool in_list(std::string_view v, std::string_view const (&arr)[N]) {
  for (std::string_view const e : arr)
    if (v == e) return true;
  return false;
}

// 词尾（去空白）最后一个字符 —— 用来识别 "2>&1" / "&>" 里的 & 是重定向的一部分
char last_non_space(std::string const& s) {
  for (std::size_t i = s.size(); i > 0; --i)
    if (!space(s[i - 1])) return s[i - 1];
  return '\0';
}

// ---- 命令表（词面保守集合，互斥）----

// 只读白名单：这些命令词面无写能力（个别带写参数的命令如 find/sed/git 走单独细查）
std::string_view constexpr kReadOnlyCmds[] = {
    "cat", "ls",     "echo",     "pwd",     "head",   "tail",   "grep",  "wc",
    "which", "diff",  "stat",     "du",      "date",   "true",   "false", "test",
    "[",     "printf", "basename", "dirname", "readlink", "cd",    "sort", "uniq",
    "cut",    "tr",    "awk",     "seq",     "whoami", "uname"};

// 危险签名：默认不可放行（其中 sudo/kill/curl/解释器 连"看起来只读"都不可信）
std::string_view constexpr kRiskyCmds[] = {
    "sudo",     "su",       "mkfs",     "mkfs.ext2", "mkfs.ext3", "mkfs.ext4",
    "mkfs.xfs", "mkfs.btrfs", "fdisk",   "parted",   "gdisk",     "shred",
    "dd",       "systemctl", "shutdown", "halt",     "poweroff",  "reboot",
    "passwd",   "useradd",  "userdel",  "usermod",   "chpasswd",  "kill",
    "killall",  "pkill",    "apt",      "apt-get",   "dnf",       "yum",
    "pacman",   "apk",      "snap",     "docker",    "podman",    "kubectl",
    "curl",     "wget",     "wget2",    "ssh",       "scp",       "sftp",
    "rsync",    "nc",       "ncat",     "telnet",    "socat",     "iptables",
    "nft",      "ufw",      "eval",     "python",    "python2",   "python3",
    "node",     "nodejs",   "ruby",     "perl",      "php",       "java",
    "go",       "cargo",    "npm",      "npx",       "yarn",      "pnpm",
    "bun",      "pip",      "pip3",     "conda",     "poetry",    "gradle",
    "mvn"};

// 普通写命令：有文件系统副作用但未见更高危签名
std::string_view constexpr kWriteCmds[] = {
    "rm",     "rmdir", "mv",      "cp",       "mkdir",   "touch",   "ln",
    "mktemp", "install", "tee",   "unzip",    "zip",     "tar",     "gzip",
    "gunzip", "xz",    "zstd",    "chmod",    "chown",   "chgrp",   "setfacl",
    "crontab", "logger", "write", "wall",     "base64",  "openssl", "make",
    "cmake",  "ctest"};

// 这些命令在"未加引号的通配符"面前可能展开成写/删参数 → 升级为 Write（同理 Claude Code）
std::string_view constexpr kGlobSensitive[]{"grep", "rg", "sed", "awk", "find", "sort", "git"};

// git 子命令：只读 / 写 / 危险
std::string_view constexpr kGitRead[]{"status",  "diff",     "log",     "show",    "rev-parse",
                                      "ls-files", "grep",    "blame",    "describe", "name-rev",
                                      "diff-tree", "diff-index"};
std::string_view constexpr kGitWrite[]{"add",        "commit",   "push",      "pull",    "fetch",
                                       "checkout",   "switch",   "restore",   "reset",   "merge",
                                       "rebase",     "cherry-pick", "stash",   "mv",      "rm",
                                       "apply",      "am",       "init",      "clone",   "submodule",
                                       "worktree",   "gc",       "prune",     "update-index",
                                       "update-ref", "notes",    "remote",    "pack-refs"};
std::string_view constexpr kGitRisky[]{"clean", "filter-branch"};

std::string_view constexpr kShellNames[]{"sh", "bash", "zsh", "dash", "ksh"};

// 透明 wrapper：本身无含义、把后续命令提升为"被包装的命令"（参数由被包装命令消化）
std::string_view constexpr kTransparentWrappers[]{"command", "builtin", "noglob", "exec", "nohup"};

// ---- 扫描：单遍引号感知拆分 + 重定向/替换探测 ----

struct Scan {
  std::vector<std::string> segments;  // 顶层操作符（&& || ; | |& & 换行）切开后的片段
  bool output_redirect = false;       // 未加引号的 > / >> / 2> / &>（写重定向）
  bool substitution = false;          // $() 或反引号（含双引号内）——无法静态分析
};

Scan scan_command(std::string_view cmd) {
  Scan s;
  std::string cur;
  char q = 0;  // 0 / '\'' / '"'
  bool esc = false;
  std::size_t const n = cmd.size();
  std::size_t i = 0;

  auto flush = [&] {
    std::string_view t = trim_view(cur);
    if (!t.empty()) s.segments.emplace_back(t);
    cur.clear();
  };

  while (i < n) {
    char const c = cmd[i];
    if (esc) { cur += c; esc = false; ++i; continue; }
    if (q == '\'') {  // 单引号内：一切字面，只认收引号
      cur += c;
      if (c == '\'') q = 0;
      ++i;
      continue;
    }
    if (q == '"') {   // 双引号内：反引号/$( 仍是替换；\" 需转义
      if (c == '\\' && i + 1 < n) { cur += c; cur += cmd[i + 1]; i += 2; continue; }
      cur += c;
      if (c == '`') s.substitution = true;
      if (c == '$' && i + 1 < n && cmd[i + 1] == '(') s.substitution = true;
      if (c == '"') q = 0;
      ++i;
      continue;
    }
    // 未加引号
    if (c == '\\') { esc = true; ++i; continue; }
    if (c == '\'' || c == '"') { q = c; cur += c; ++i; continue; }
    if (c == '`' || (c == '$' && i + 1 < n && cmd[i + 1] == '(')) {
      s.substitution = true;
      cur += c;
      ++i;
      continue;
    }
    if (c == '>') {  // 写重定向（含 >、>>、2>…；'&>' 由 '&' 分支兜住）
      s.output_redirect = true;
      cur += c;
      ++i;
      if (i < n && cmd[i] == '>') { cur += '>'; ++i; }
      continue;
    }
    if (c == '&') {
      if (last_non_space(cur) == '>' || (i + 1 < n && cmd[i + 1] == '>')) {
        // "2>&1" 的 & 或 "cmd &> f"：重定向的一部分，不是后台操作符
        cur += c;
        ++i;
        if (i < n && cmd[i] == '>') { s.output_redirect = true; cur += '>'; ++i; }
        continue;
      }
      if (i + 1 < n && cmd[i + 1] == '&') { flush(); i += 2; continue; }
      flush(); ++i; continue;  // 后台 &
    }
    if (c == '|') {
      if (i + 1 < n && cmd[i + 1] == '|') { flush(); i += 2; continue; }
      flush(); ++i; continue;
    }
    if (c == ';' || c == '\n') { flush(); ++i; continue; }
    if (c == '#' && (cur.empty() || space(cur.back()))) {
      flush();  // 注释：从词首开始，其后的行内内容全部忽略
      while (i < n && cmd[i] != '\n') ++i;
      continue;
    }
    cur += c;
    ++i;
  }
  flush();
  return s;
}

// ---- 片段 → 词（去引号），同时记录"未加引号的通配符" ----

struct Words {
  std::vector<std::string> toks;
  bool unquoted_glob = false;  // 词里有未加引号的 * ? [
};

Words parse_words(std::string_view seg) {
  Words w;
  std::string cur;
  char q = 0;
  bool esc = false;
  for (std::size_t i = 0; i < seg.size(); ++i) {
    char const c = seg[i];
    if (esc) { cur += c; esc = false; continue; }
    if (c == '\\' && q != '\'') { esc = true; continue; }
    if (q) {
      if (c == q) { q = 0; continue; }
      cur += c;
      continue;
    }
    if (c == '\'' || c == '"') { q = c; continue; }
    if (space(c)) {
      if (!cur.empty()) { w.toks.push_back(cur); cur.clear(); }
      continue;
    }
    if (c == '*' || c == '?' || c == '[') w.unquoted_glob = true;
    cur += c;
  }
  if (!cur.empty()) w.toks.push_back(cur);
  return w;
}

std::string join_words(std::vector<std::string> const& v, std::size_t from) {
  std::string out;
  for (std::size_t i = from; i < v.size(); ++i) {
    if (!out.empty()) out += ' ';
    out += v[i];
  }
  return out;
}

// ---- wrapper 剥离（启发式）----

std::vector<std::string> strip_wrappers(std::vector<std::string> toks) {
  std::size_t guard = 0;
  while (!toks.empty() && guard++ < 8) {
    std::string const& w = toks.front();
    if (in_list(w, kTransparentWrappers)) { toks.erase(toks.begin()); continue; }
    if (w == "timeout") {
      toks.erase(toks.begin());
      if (!toks.empty() && all_digits(toks.front())) toks.erase(toks.begin());
      else if (!toks.empty() && (toks.front() == "-s" || toks.front().starts_with("--signal"))) {
        toks.erase(toks.begin());
        if (!toks.empty() && (all_digits(toks.front()) || toks.front().size() >= 3))
          toks.erase(toks.begin());  // SIG 名/数字
        if (!toks.empty() && all_digits(toks.front())) toks.erase(toks.begin());  // 时长
      } else if (!toks.empty() && toks.front().starts_with('-')) {
        toks.erase(toks.begin());  // 单旗标簇（启发式：不深究）
        if (!toks.empty() && all_digits(toks.front())) toks.erase(toks.begin());
      }
      continue;
    }
    if (w == "nice") {
      toks.erase(toks.begin());
      if (!toks.empty() && toks.front() == "-n") {
        toks.erase(toks.begin());
        if (!toks.empty() && all_digits(toks.front())) toks.erase(toks.begin());
      }
      continue;
    }
    break;
  }
  return toks;
}

// ---- 分类核心 ----

CommandClassify classify_words(Words w, int depth);

// git 子命令细查（参数级旗标的保守集合）
CommandClassify classify_git(std::vector<std::string> const& toks, bool glob) {
  if (toks.size() < 2) return {CommandClass::Unknown, "git 缺子命令"};
  std::string const lsub = ascii_lower(toks[1]);

  if (in_list(lsub, kGitRisky)) return {CommandClass::Risky, "git 危险子命令: " + toks[1]};
  if (glob && in_list(lsub, kGitRead))
    return {CommandClass::Write, "git " + toks[1] + " 引用了未加引号的通配符"};

  // 破坏性旗标叠加（reset/checkout/restore/branch/tag/push 的 -f/-D/--hard 等）
  if (lsub == "reset" || lsub == "checkout" || lsub == "restore" || lsub == "branch" ||
      lsub == "tag" || lsub == "push") {
    for (std::size_t i = 2; i < toks.size(); ++i) {
      std::string_view a = toks[i];
      if (a == "--hard" || a == "-f" || a == "--force" || a == "-D" || a == "-d" ||
          a == "--delete" || a == "-m" || a == "--move")
        return {CommandClass::Risky, "git " + toks[1] + " 带破坏性旗标: " + std::string(a)};
    }
  }

  if (lsub == "branch" || lsub == "tag") {
    // 纯列出（无非旗标参数）→ 只读；其余（创建/删除/改名）→ 写
    for (std::size_t i = 2; i < toks.size(); ++i) {
      std::string_view a = toks[i];
      if (a.size() < 2 || a[0] != '-') return {CommandClass::Write, "git " + toks[1] + " 修改分支/标签: " + toks[i]};
    }
    return {CommandClass::ReadOnly, "git " + toks[1] + " 只读列出"};
  }
  if (lsub == "config") {
    for (std::size_t i = 2; i < toks.size(); ++i) {
      std::string_view a = toks[i];
      if (a == "--set" || a == "--add" || a == "--unset" || a == "--unset-all" ||
          a == "--replace-all" || a == "--edit" || a == "--global" || a == "--local" ||
          a == "--system")
        return {CommandClass::Write, "git config 写配置"};
    }
    if (toks.size() == 3 || toks.size() == 2) return {CommandClass::ReadOnly, "git config 读出"};
    return {CommandClass::Unknown, "git config 形式未识别"};
  }
  if (lsub == "remote") {
    for (std::size_t i = 2; i < toks.size(); ++i) {
      std::string_view a = toks[i];
      if (a != "-v" && a != "-a" && a != "--verbose" && a != "--all")
        return {CommandClass::Write, "git remote 修改"};
    }
    return {CommandClass::ReadOnly, "git remote -v/-a 只读列出"};
  }

  if (in_list(lsub, kGitWrite)) return {CommandClass::Write, "git 写子命令: " + toks[1]};
  if (in_list(lsub, kGitRead)) return {CommandClass::ReadOnly, "git 只读子命令: " + toks[1]};
  return {CommandClass::Unknown, "git 子命令未识别: " + toks[1]};
}

// 顶层分类：脚本级（扫描 → 逐片段 → 取最坏类别）
CommandClassify classify_script(std::string_view script, int depth);

CommandClassify classify_words(Words w, int depth) {
  std::vector<std::string> toks = strip_wrappers(w.toks);
  if (toks.empty()) return {CommandClass::Unknown, "只有 wrapper，无可执行命令"};

  // env 特判：env 单独 = 列环境（只读）；含 '=' 赋值 = 修改环境；env <cmd> = 透明前缀
  if (toks[0] == "env") {
    if (toks.size() == 1) return {CommandClass::ReadOnly, "env 列出环境变量"};
    for (std::string const& t : toks)
      if (t.find('=') != std::string::npos) return {CommandClass::Write, "env 设置环境变量后执行"};
    Words rest{.toks = std::vector<std::string>(toks.begin() + 1, toks.end()),
               .unquoted_glob = w.unquoted_glob};
    return classify_words(std::move(rest), depth);
  }

  // 嵌套解析：sh/bash/zsh -c '<脚本>' → 递归解析脚本本体（脚本级，而非词级）
  if (toks.size() >= 2 && toks[1] == "-c" && in_list(toks[0], kShellNames)) {
    if (depth >= 3) return {CommandClass::Unknown, "嵌套 shell 过深"};
    return classify_script(join_words(toks, 2), depth + 1);
  }

  std::string const cmd = toks[0];
  std::string const lc = ascii_lower(cmd);

  // 未加引号通配符 + glob 敏感的只读命令 → 升级为 Write
  if (w.unquoted_glob && in_list(lc, kGlobSensitive))
    return {CommandClass::Write, std::string("命令 ") + lc + " 带未加引号的通配符"};

  if (lc == "eval") return {CommandClass::Risky, "eval 无法静态分析"};

  // rm/rmdir 带递归/强删旗标 → 危险（rm -rf 是经典不可逆操作）
  if (lc == "rm" || lc == "rmdir") {
    for (std::string const& t : toks)
      if (t.size() > 1 && t[0] == '-' &&
          (t.find('r') != std::string::npos || t.find('R') != std::string::npos ||
           t.find('f') != std::string::npos))
        return {CommandClass::Risky, std::string("rm/rmdir 带强删旗标: ") + t};
    return {CommandClass::Write, "rm/rmdir 普通删除（无递归/强删）"};
  }

  if (in_list(lc, kRiskyCmds)) return {CommandClass::Risky, "危险命令: " + cmd};

  if (lc == "find") {
    for (std::string const& t : toks)
      if (t == "-exec" || t == "-ok" || t == "-delete") return {CommandClass::Risky, "find 高危动作: " + t};
    return {CommandClass::ReadOnly, "find 遍历文件树（只读形式）"};
  }
  if (lc == "sed") {
    for (std::string const& t : toks)
      if (t.size() >= 2 && t[0] == '-' && t[1] == 'i' && (t.size() == 2 || t[2] != '-'))
        return {CommandClass::Write, "sed -i 就地写文件"};
    return {CommandClass::ReadOnly, "sed 只读流编辑"};
  }
  if (lc == "git") return classify_git(toks, w.unquoted_glob);
  if (in_list(lc, kReadOnlyCmds)) return {CommandClass::ReadOnly, std::string("命令 ") + lc + " 在只读白名单内"};
  if (in_list(lc, kWriteCmds)) return {CommandClass::Write, "写命令: " + cmd};
  return {CommandClass::Unknown, "未知命令: " + cmd};
}

CommandClassify classify_script(std::string_view script, int depth) {
  CommandClassify total{CommandClass::ReadOnly, "所有子命令词面可证只读"};
  if (trim_view(script).empty()) return {CommandClass::Unknown, "空命令"};

  Scan const sc = scan_command(script);
  if (sc.substitution) return {CommandClass::Unknown, "含命令替换（$() 或反引号），无法静态分析"};
  if (sc.output_redirect) total = {CommandClass::Write, "含输出重定向（> / >> / &>）"};

  for (std::string const& seg : sc.segments) {
    Words w = parse_words(seg);
    if (w.toks.empty()) continue;
    CommandClassify const c = classify_words(std::move(w), depth);
    if (static_cast<int>(c.cls) > static_cast<int>(total.cls)) total = c;
    if (total.cls == CommandClass::Unknown) break;  // fail-closed：最坏情况短路
  }
  return total;
}

}  // namespace

CommandClass classify_bash_command(std::string_view command) {
  return classify_bash_command_detail(command).cls;
}

CommandClassify classify_bash_command_detail(std::string_view command) {
  return classify_script(command, /*depth=*/0);
}

char const* command_class_name(CommandClass cl) {
  switch (cl) {
    case CommandClass::ReadOnly: return "ReadOnly";
    case CommandClass::Write: return "Write";
    case CommandClass::Risky: return "Risky";
    case CommandClass::Unknown: return "Unknown";
  }
  return "Unknown";
}

}  // namespace cppchain::tool