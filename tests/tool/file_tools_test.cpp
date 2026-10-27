// M3 测试：文件系统工具三件套（file_read / file_write / list_dir）
// 用临时目录做沙箱根；验证落盘正确性、越界三类拒绝（../、绝对路径、符号链接逃逸）、
// 副作用标记（file_write = read_only=false）。
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "cppchain/tool/file_tools.hpp"

using namespace cppchain;
using namespace cppchain::tool;
using nlohmann::json;

namespace {

std::atomic<int> g_counter{0};

class FileToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("cppchain_ft_" + std::to_string(++g_counter));
    std::filesystem::create_directories(root_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  FileToolsConfig make_cfg() {
    FileToolsConfig c;
    c.root_dir = root_;
    return c;
  }
  ToolSpec make_spec(std::string name) {
    auto tools = make_file_tools(make_cfg());
    for (auto const& t : tools)
      if (t.name == name) return t;
    throw std::runtime_error("no such tool: " + name);
  }
  ToolContext make_ctx() {
    ToolContext c;
    c.tool_call_id = "call_f";
    return c;
  }

  // 在沙箱里落一个文件，返回其内容（供读回校验）
  void write_file(std::filesystem::path rel, std::string content) {
    std::filesystem::create_directories((root_ / rel).parent_path());
    std::ofstream out(root_ / rel, std::ios::binary);
    out << content;
  }
  std::string read_raw(std::filesystem::path rel) {
    std::ifstream in(root_ / rel, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{});
  }

  std::filesystem::path root_;
};

}  // namespace

// ---- resolve_path_in_root：安全边界 ----

TEST_F(FileToolsTest, ResolveNormalizesWithinRoot) {
  auto p = resolve_path_in_root(root_, "a/b");
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(*p, (root_ / "a" / "b").lexically_normal());
  EXPECT_EQ(*resolve_path_in_root(root_, "."), root_.lexically_normal());
  EXPECT_EQ(*resolve_path_in_root(root_, ""), root_.lexically_normal());  // 空 → 根
}

TEST_F(FileToolsTest, ResolveRejectsTraversalAndAbsolute) {
  EXPECT_FALSE(resolve_path_in_root(root_, "../x"));
  EXPECT_FALSE(resolve_path_in_root(root_, "a/../../b"));
  EXPECT_FALSE(resolve_path_in_root(root_, ".."));
  EXPECT_FALSE(resolve_path_in_root(root_, "/etc/passwd"));
  EXPECT_FALSE(resolve_path_in_root(root_, "a/../.."));
}

TEST_F(FileToolsTest, ResolveRejectsSiblingPrefixConfusion) {
  // root 名 "ccbase"、邻居 "ccbaseX"：词法包含必须按 '/' 边界，不能 "ccbaseX" 误算在根内
  auto const parent = root_.parent_path();
  std::filesystem::path const base = parent / "ccbase";
  std::filesystem::create_directories(base);
  std::filesystem::create_directory(parent / "ccbaseX");
  EXPECT_FALSE(resolve_path_in_root(base, "../ccbaseX/m").has_value());
  EXPECT_TRUE(resolve_path_in_root(base, "m").has_value());
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::remove(parent / "ccbaseX", ec);
}

TEST_F(FileToolsTest, ResolveRejectsSymlinkEscape) {
  auto const outside = root_.parent_path() / ("outside_" + root_.filename().string());
  std::error_code ec;
  std::filesystem::create_directories(outside, ec);
  std::filesystem::create_symlink(outside, root_ / "link", ec);
  EXPECT_FALSE(resolve_path_in_root(root_, "link/x").has_value());  // 经链接逃出 → 拒绝
  EXPECT_TRUE(resolve_path_in_root(root_, ".").has_value());        // 根本身不受影响
  std::filesystem::remove_all(outside, ec);
  std::filesystem::remove(root_ / "link", ec);
}

// ---- file_write ----

TEST_F(FileToolsTest, WriteCreatesFileAndParentDirs) {
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "notes/plan.md"}, {"content", "hello"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("path").get<std::string>(), "notes/plan.md");
  EXPECT_EQ(r->at("bytes_written").get<std::size_t>(), 5u);
  EXPECT_TRUE(r->at("created").get<bool>());
  EXPECT_EQ(r->at("mode").get<std::string>(), "overwrite");
  EXPECT_EQ(read_raw("notes/plan.md"), "hello");  // 落盘验证
}

TEST_F(FileToolsTest, WriteAppendsWhenAsked) {
  write_file("a.txt", "1");
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}, {"content", "2"}, {"append", true}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(r->at("created").get<bool>());
  EXPECT_EQ(r->at("mode").get<std::string>(), "append");
  EXPECT_EQ(read_raw("a.txt"), "12");
}

TEST_F(FileToolsTest, WriteOverwritesByDefault) {
  write_file("a.txt", "old");
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}, {"content", "new"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("mode").get<std::string>(), "overwrite");
  EXPECT_EQ(read_raw("a.txt"), "new");
}

TEST_F(FileToolsTest, WriteRejectsTraversalAndWritesNothing) {
  auto const escape_file = root_.parent_path() / ("esc_outside_" + root_.filename().string());
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(
      json{{"path", "../" + escape_file.filename().string()}, {"content", "evil"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(escape_file));  // 确认根本没写出去
}

TEST_F(FileToolsTest, WriteRejectsAbsolutePath) {
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "/etc/evil"}, {"content", "x"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("相对"), std::string::npos);
}

TEST_F(FileToolsTest, WriteRejectsDirectoryTarget) {
  auto spec = make_spec("file_write");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "."}, {"content", "x"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("目录"), std::string::npos);
}

TEST_F(FileToolsTest, WriteRejectsOversizedContent) {
  FileToolsConfig cfg = make_cfg();
  cfg.max_write_chars = 10;
  auto spec = make_file_write_tool(cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "big.txt"}, {"content", std::string(20, 'a')}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("过长"), std::string::npos);
}

// ---- file_read ----

TEST_F(FileToolsTest, ReadRoundTrip) {
  write_file("a.txt", "一二三456");
  auto spec = make_spec("file_read");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("path").get<std::string>(), "a.txt");
  EXPECT_EQ(r->at("content").get<std::string>(), "一二三456");
  EXPECT_EQ(r->at("size").get<long long>(), 12);  // "一二三"3 字节*3 + "456"3 字节 = 12
  EXPECT_FALSE(r->at("truncated").get<bool>());
}

TEST_F(FileToolsTest, ReadTruncatesUtf8Safely) {
  write_file("t.txt", "一二三四五六七八九十");
  FileToolsConfig cfg = make_cfg();
  cfg.default_read_chars = 4;
  auto spec = make_file_read_tool(cfg);
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "t.txt"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("content").get<std::string>(), "一…");  // 边界回退到完整字符
  EXPECT_EQ(r->at("size").get<long long>(), 30);
  EXPECT_TRUE(r->at("truncated").get<bool>());
}

TEST_F(FileToolsTest, ReadMissingFileIsError) {
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "nope.txt"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("不存在"), std::string::npos);
}

TEST_F(FileToolsTest, ReadDirectoryIsError) {
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "."}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("目录"), std::string::npos);
}

TEST_F(FileToolsTest, ReadRejectsBinary) {
  {
    std::ofstream out(root_ / "bin.dat", std::ios::binary);
    std::string const s = std::string("A") + '\0' + "B";
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "bin.dat"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("二进制"), std::string::npos);
}

TEST_F(FileToolsTest, ReadRejectsTraversal) {
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "../../passwd"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos);
}

TEST_F(FileToolsTest, MissingPathIsArgError) {
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("path"), std::string::npos);
}

// ---- file_read 分页（problems/02 追加 S1：offset / next_offset）----

TEST_F(FileToolsTest, ReadOffsetWindowAndNextOffset) {
  write_file("a.txt", "0123456789");
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}, {"offset", 3}, {"max_chars", 4}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("content").get<std::string>(), "3456…");  // 「…」是截断标记，不是文件内容
  EXPECT_EQ(r->at("offset").get<long long>(), 3);
  EXPECT_EQ(r->at("next_offset").get<long long>(), 7);
  EXPECT_TRUE(r->at("truncated").get<bool>());
  EXPECT_EQ(r->at("size").get<long long>(), 10);
}

TEST_F(FileToolsTest, ReadOffsetAtEndIsEmptyAndDone) {
  write_file("a.txt", "abc");
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}, {"offset", 3}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("content").get<std::string>(), "");
  EXPECT_FALSE(r->at("truncated").get<bool>());
  EXPECT_TRUE(r->at("next_offset").is_null());
}

TEST_F(FileToolsTest, ReadOffsetBeyondSizeIsError) {
  write_file("a.txt", "abc");
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "a.txt"}, {"offset", 99}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("offset"), std::string::npos);
}

TEST_F(FileToolsTest, ReadOffsetSnapsToUtf8Boundary) {
  write_file("u.txt", "一二三");  // 9 字节
  auto spec = make_spec("file_read");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "u.txt"}, {"offset", 1}}, ctx);  // 落在「一」的续字节上
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("offset").get<long long>(), 3);  // 前移到下一个字符边界
  EXPECT_EQ(r->at("content").get<std::string>(), "二三");
  EXPECT_FALSE(r->at("truncated").get<bool>());
  EXPECT_TRUE(r->at("next_offset").is_null());
}

TEST_F(FileToolsTest, ReadPagingReconstructsWholeFile) {
  std::string const full = "第一段：abc\n第二段：def\n第三行结束";
  write_file("page.txt", full);
  auto spec = make_spec("file_read");
  ToolContext ctx = make_ctx();

  std::string rebuilt;
  long long offset = 0;
  for (int guard = 0; guard < 100; ++guard) {
    auto r = spec.handler(
        json{{"path", "page.txt"}, {"offset", offset}, {"max_chars", 7}}, ctx);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    std::string content = r->at("content").get<std::string>();
    bool const truncated = r->at("truncated").get<bool>();
    if (truncated) {
      ASSERT_FALSE(content.empty());
      ASSERT_TRUE(content.ends_with("…"));
      content.resize(content.size() - std::string("…").size());  // 「…」不在文件里
      ASSERT_FALSE(r->at("next_offset").is_null());
      long long const next = r->at("next_offset").get<long long>();
      EXPECT_GT(next, offset);  // 必须前进，否则死循环
      rebuilt += content;
      offset = next;
      continue;
    }
    rebuilt += content;
    EXPECT_TRUE(r->at("next_offset").is_null());
    break;
  }
  EXPECT_EQ(rebuilt, full);
}

// ---- list_dir ----

TEST_F(FileToolsTest, ListEmptyDir) {
  auto spec = make_spec("list_dir");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("path").get<std::string>(), ".");  // 缺省列根目录
  EXPECT_TRUE(r->at("entries").is_array());
  EXPECT_EQ(r->at("entries").size(), 0u);
  EXPECT_FALSE(r->at("truncated").get<bool>());
}

TEST_F(FileToolsTest, ListSortsEntriesWithTypesAndSizes) {
  write_file("b.txt", "bb");
  write_file("a.txt", "aa");
  std::filesystem::create_directory(root_ / "sub");
  auto spec = make_spec("list_dir");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "."}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  auto const& es = r->at("entries");
  ASSERT_EQ(es.size(), 3u);
  EXPECT_EQ(es[0].at("name").get<std::string>(), "a.txt");  // 按名称排序
  EXPECT_EQ(es[0].at("type").get<std::string>(), "file");
  EXPECT_EQ(es[0].at("size").get<long long>(), 2);
  EXPECT_EQ(es[1].at("name").get<std::string>(), "b.txt");
  EXPECT_EQ(es[2].at("name").get<std::string>(), "sub");
  EXPECT_EQ(es[2].at("type").get<std::string>(), "dir");
}

TEST_F(FileToolsTest, ListTruncatesAboveMaxEntries) {
  write_file("1.txt", "x");
  write_file("2.txt", "x");
  write_file("3.txt", "x");
  auto spec = make_spec("list_dir");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "."}, {"max_entries", 2}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_TRUE(r->at("truncated").get<bool>());
  EXPECT_EQ(r->at("entries").size(), 2u);
}

TEST_F(FileToolsTest, ListMissingDirIsError) {
  auto spec = make_spec("list_dir");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "nope"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("不存在"), std::string::npos);
}

TEST_F(FileToolsTest, ListRejectsTraversal) {
  auto spec = make_spec("list_dir");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", ".."}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos);
}

// ---- file_grep（problems/02 追加 S2：确定性定位）----

TEST_F(FileToolsTest, GrepFindsMatchesWithLineNumbers) {
  write_file("a.txt", "alpha\nbeta ERROR here\ngamma\n");
  write_file("sub/b.log", "ok\nERROR second\n");
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "ERROR"}, {"path", "."}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("match_count").get<long>(), 2);
  EXPECT_EQ(r->at("files_searched").get<long>(), 2);
  EXPECT_FALSE(r->at("truncated").get<bool>());
  auto const& m = r->at("matches");
  ASSERT_EQ(m.size(), 2u);
  EXPECT_EQ(m[0].at("path").get<std::string>(), "a.txt");  // 路径字典序
  EXPECT_EQ(m[0].at("line").get<long>(), 2);               // 行号从 1 起
  EXPECT_EQ(m[0].at("text").get<std::string>(), "beta ERROR here");
  EXPECT_EQ(m[1].at("path").get<std::string>(), "sub/b.log");
  EXPECT_EQ(m[1].at("line").get<long>(), 2);
}

TEST_F(FileToolsTest, GrepSingleFileAndIgnoreCase) {
  write_file("a.txt", "first\nSecond Error\nthird\n");
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(
      json{{"pattern", "error"}, {"path", "a.txt"}, {"ignore_case", true}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("match_count").get<long>(), 1);
  EXPECT_EQ(r->at("matches")[0].at("line").get<long>(), 2);
  EXPECT_EQ(r->at("files_searched").get<long>(), 1);
}

TEST_F(FileToolsTest, GrepRespectsMaxMatches) {
  write_file("a.txt", "hit 1\nhit 2\nhit 3\n");
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "hit"}, {"max_matches", 2}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("match_count").get<long>(), 2);
  EXPECT_TRUE(r->at("truncated").get<bool>());
}

TEST_F(FileToolsTest, GrepInvalidRegexIsError) {
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "([unclosed"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("正则"), std::string::npos);
}

TEST_F(FileToolsTest, GrepSkipsBinaryAndReportsIt) {
  write_file("a.txt", "ok\nERROR text\n");
  {
    std::ofstream out(root_ / "bin.dat", std::ios::binary);
    std::string const s = std::string("ERROR") + '\0' + "tail";
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
  }
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "ERROR"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("match_count").get<long>(), 1);  // 二进制里的命中不算
  EXPECT_EQ(r->at("files_skipped").get<long>(), 1);  // 但如实报告跳过
}

TEST_F(FileToolsTest, GrepHonoursTotalScanBudget) {
  write_file("a.txt", "nomatch\n");
  write_file("b.txt", "TARGET\n");  // 本应命中，但总扫描预算在扫完 a 后已耗尽
  FileToolsConfig cfg = make_cfg();
  cfg.max_grep_scan_bytes = 1;
  auto spec = make_file_grep_tool(cfg);

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "TARGET"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(r->at("match_count").get<long>(), 0);
  EXPECT_EQ(r->at("files_searched").get<long>(), 1);
  EXPECT_TRUE(r->at("truncated").get<bool>());  // 不是"没有"，而是"没扫完"
}

TEST_F(FileToolsTest, GrepSkipsRuntimeDirs) {
  // .wfr（会话日志/产物）与 .git 不进搜索面：否则"搜到自己刚写的提问"会污染路由
  write_file(".wfr/sessions/s.log", "NEEDLE inside runtime state\n");
  write_file("a.txt", "NEEDLE real\n");
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "NEEDLE"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  ASSERT_EQ(r->at("match_count").get<long>(), 1);
  EXPECT_EQ(r->at("matches")[0].at("path").get<std::string>(), "a.txt");
}

TEST_F(FileToolsTest, GrepRejectsTraversal) {
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", "x"}, {"path", ".."}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos);
}

TEST_F(FileToolsTest, GrepEmptyPatternIsArgError) {
  auto spec = make_spec("file_grep");

  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"pattern", ""}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("pattern"), std::string::npos);
}

// ---- file_extract（problems/02 追加 S4：结构化提取）----

TEST_F(FileToolsTest, ExtractJsonPointerAndDotPath) {
  write_file("cfg.json", R"({"a":{"b":[{"c":1},{"c":2}]},"name":"x"})");
  auto spec = make_spec("file_extract");
  ToolContext ctx = make_ctx();

  auto r1 = spec.handler(json{{"path", "cfg.json"}, {"selector", "/a/b/1/c"}}, ctx);
  ASSERT_TRUE(r1.has_value()) << r1.error().message;
  EXPECT_TRUE(r1->at("found").get<bool>());
  EXPECT_EQ(r1->at("text").get<std::string>(), "2");

  auto r2 = spec.handler(json{{"path", "cfg.json"}, {"selector", "a.b[1].c"}}, ctx);
  ASSERT_TRUE(r2.has_value()) << r2.error().message;
  EXPECT_EQ(r2->at("text").get<std::string>(), "2");

  auto r3 = spec.handler(json{{"path", "cfg.json"}, {"selector", "name"}}, ctx);
  ASSERT_TRUE(r3.has_value()) << r3.error().message;
  EXPECT_EQ(r3->at("text").get<std::string>(), "x");

  auto r4 = spec.handler(json{{"path", "cfg.json"}, {"selector", "a.missing"}}, ctx);
  ASSERT_TRUE(r4.has_value()) << r4.error().message;
  EXPECT_FALSE(r4->at("found").get<bool>());
}

TEST_F(FileToolsTest, ExtractJsonInvalidIsError) {
  write_file("bad.json", "not json");
  auto spec = make_spec("file_extract");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "bad.json"}, {"selector", "a"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("JSON"), std::string::npos);
}

TEST_F(FileToolsTest, ExtractYamlKeyPaths) {
  write_file("app.yaml",
             "server:\n  host: localhost\n  ports:\n    - 80\n    - 443\nname: \"web\"\n");
  auto spec = make_spec("file_extract");
  ToolContext ctx = make_ctx();

  auto r1 = spec.handler(json{{"path", "app.yaml"}, {"selector", "server.host"}}, ctx);
  ASSERT_TRUE(r1.has_value()) << r1.error().message;
  EXPECT_TRUE(r1->at("found").get<bool>());
  EXPECT_EQ(r1->at("text").get<std::string>(), "localhost");

  auto r2 = spec.handler(json{{"path", "app.yaml"}, {"selector", "server.ports.1"}}, ctx);
  ASSERT_TRUE(r2.has_value()) << r2.error().message;
  EXPECT_EQ(r2->at("text").get<std::string>(), "443");

  auto r3 = spec.handler(json{{"path", "app.yaml"}, {"selector", "name"}}, ctx);
  ASSERT_TRUE(r3.has_value()) << r3.error().message;
  EXPECT_EQ(r3->at("text").get<std::string>(), "web");

  auto r4 = spec.handler(json{{"path", "app.yaml"}, {"selector", "server.ports"}}, ctx);
  ASSERT_TRUE(r4.has_value()) << r4.error().message;
  ASSERT_TRUE(r4->at("found").get<bool>());
  EXPECT_NE(r4->at("text").get<std::string>().find("80"), std::string::npos);
  EXPECT_NE(r4->at("text").get<std::string>().find("443"), std::string::npos);

  auto r5 = spec.handler(json{{"path", "app.yaml"}, {"selector", "server.nope"}}, ctx);
  ASSERT_TRUE(r5.has_value()) << r5.error().message;
  EXPECT_FALSE(r5->at("found").get<bool>());
}

TEST_F(FileToolsTest, ExtractMarkdownOutlineAndSection) {
  write_file("doc.md", "# Title\nintro\n## Part A\nalpha\n### Sub A1\na1\n## Part B\nbeta\n");
  auto spec = make_spec("file_extract");
  ToolContext ctx = make_ctx();

  auto outline = spec.handler(json{{"path", "doc.md"}, {"selector", ""}}, ctx);
  ASSERT_TRUE(outline.has_value()) << outline.error().message;
  ASSERT_TRUE(outline->at("found").get<bool>());
  std::string const o = outline->at("text").get<std::string>();
  EXPECT_NE(o.find("L1 [1] Title"), std::string::npos);
  EXPECT_NE(o.find("L3 [2] Part A"), std::string::npos);
  EXPECT_NE(o.find("L5 [3] Sub A1"), std::string::npos);
  EXPECT_NE(o.find("L7 [2] Part B"), std::string::npos);

  auto a = spec.handler(json{{"path", "doc.md"}, {"selector", "part a"}}, ctx);
  ASSERT_TRUE(a.has_value()) << a.error().message;
  ASSERT_TRUE(a->at("found").get<bool>());
  std::string const ta = a->at("text").get<std::string>();
  EXPECT_NE(ta.find("## Part A"), std::string::npos);
  EXPECT_NE(ta.find("### Sub A1"), std::string::npos);  // 子节随父节返回
  EXPECT_EQ(ta.find("Part B"), std::string::npos);      // 同级下一节不进来

  auto miss = spec.handler(json{{"path", "doc.md"}, {"selector", "nope"}}, ctx);
  ASSERT_TRUE(miss.has_value()) << miss.error().message;
  EXPECT_FALSE(miss->at("found").get<bool>());
  EXPECT_NE(miss->at("hint").get<std::string>().find("Part B"), std::string::npos);
}

TEST_F(FileToolsTest, ExtractXmlElementPath) {
  write_file("data.xml",
             "<root>\n  <item id=\"1\"><name>alpha</name></item>\n"
             "  <item id=\"2\"><name>beta</name></item>\n</root>\n");
  auto spec = make_spec("file_extract");
  ToolContext ctx = make_ctx();

  auto r1 = spec.handler(json{{"path", "data.xml"}, {"selector", "root/item[2]/name"}}, ctx);
  ASSERT_TRUE(r1.has_value()) << r1.error().message;
  ASSERT_TRUE(r1->at("found").get<bool>());
  EXPECT_EQ(r1->at("text").get<std::string>(), "beta");

  auto r2 = spec.handler(json{{"path", "data.xml"}, {"selector", "root/item/name"}}, ctx);
  ASSERT_TRUE(r2.has_value()) << r2.error().message;
  EXPECT_EQ(r2->at("text").get<std::string>(), "alpha");

  auto outline = spec.handler(json{{"path", "data.xml"}, {"selector", ""}}, ctx);
  ASSERT_TRUE(outline.has_value()) << outline.error().message;
  EXPECT_NE(outline->at("text").get<std::string>().find("root/item[2]/name"),
            std::string::npos);

  auto miss = spec.handler(json{{"path", "data.xml"}, {"selector", "root/missing"}}, ctx);
  ASSERT_TRUE(miss.has_value()) << miss.error().message;
  EXPECT_FALSE(miss->at("found").get<bool>());
}

// ---- file_sample（problems/02 追加 S5：采样跳读）----

TEST_F(FileToolsTest, SampleSmallFileReturnsWhole) {
  write_file("s.txt", "hello");
  auto spec = make_spec("file_sample");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "s.txt"}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  auto const& w = r->at("windows");
  ASSERT_EQ(w.size(), 1u);
  EXPECT_EQ(w[0].at("where").get<std::string>(), "whole");
  EXPECT_EQ(w[0].at("text").get<std::string>(), "hello");
}

TEST_F(FileToolsTest, SampleLargeFileReturnsHeadMiddleTail) {
  std::string big;
  for (int i = 0; i < 1000; ++i) big += "0123456789";  // 10KB
  write_file("big.txt", big);
  auto spec = make_spec("file_sample");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "big.txt"}, {"chars_per_window", 100}}, ctx);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  auto const& w = r->at("windows");
  ASSERT_EQ(w.size(), 3u);
  EXPECT_EQ(w[0].at("where").get<std::string>(), "head");
  EXPECT_EQ(w[1].at("where").get<std::string>(), "middle");
  EXPECT_EQ(w[2].at("where").get<std::string>(), "tail");
  EXPECT_EQ(w[0].at("offset").get<long long>(), 0);
  EXPECT_GT(w[1].at("offset").get<long long>(), 0);
  EXPECT_GT(w[2].at("offset").get<long long>(), w[1].at("offset").get<long long>());
  EXPECT_EQ(w[0].at("text").get<std::string>(), big.substr(0, 100));
  EXPECT_EQ(w[2].at("text").get<std::string>(), big.substr(big.size() - 100));
  EXPECT_NE(r->at("note").get<std::string>().find("不保证覆盖"), std::string::npos);
}

TEST_F(FileToolsTest, SampleRejectsTraversal) {
  auto spec = make_spec("file_sample");
  ToolContext ctx = make_ctx();
  auto r = spec.handler(json{{"path", "../x"}}, ctx);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("越界"), std::string::npos);
}

// ---- 副作用语义 ----

TEST_F(FileToolsTest, ReadOnlyFlagsMatchSideEffectSemantics) {
  EXPECT_TRUE(make_file_read_tool(make_cfg()).read_only);
  EXPECT_TRUE(make_list_dir_tool(make_cfg()).read_only);
  EXPECT_TRUE(make_file_grep_tool(make_cfg()).read_only);
  EXPECT_TRUE(make_file_extract_tool(make_cfg()).read_only);
  EXPECT_TRUE(make_file_sample_tool(make_cfg()).read_only);
  EXPECT_FALSE(make_file_write_tool(make_cfg()).read_only);  // 写 = 副作用，走审批 + 防重放
}

TEST_F(FileToolsTest, SnapshotTtlOnlyOnReadTools) {
  EXPECT_TRUE(make_file_read_tool(make_cfg()).ttl.has_value());
  EXPECT_TRUE(make_list_dir_tool(make_cfg()).ttl.has_value());
  EXPECT_TRUE(make_file_grep_tool(make_cfg()).ttl.has_value());
  EXPECT_TRUE(make_file_extract_tool(make_cfg()).ttl.has_value());
  EXPECT_TRUE(make_file_sample_tool(make_cfg()).ttl.has_value());
  EXPECT_FALSE(make_file_write_tool(make_cfg()).ttl.has_value());
}

TEST_F(FileToolsTest, BatchFactoryNames) {
  auto tools = make_file_tools(make_cfg());
  ASSERT_EQ(tools.size(), 6u);
  EXPECT_EQ(tools[0].name, "file_read");
  EXPECT_EQ(tools[1].name, "file_write");
  EXPECT_EQ(tools[2].name, "list_dir");
  EXPECT_EQ(tools[3].name, "file_grep");
  EXPECT_EQ(tools[4].name, "file_extract");
  EXPECT_EQ(tools[5].name, "file_sample");
}