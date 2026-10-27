// cppchain —— src/tool/file_tools.cpp
#include "cppchain/tool/file_tools.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cppchain/tool/file_tools.hpp"
#include "cppchain/tool/probe.hpp"

namespace cppchain::tool {
namespace {

// base 词法上包含 p（base 本身或 base 下任意子路径；挡"同名目录前缀"误判）
bool is_within(std::filesystem::path const& base, std::filesystem::path const& p) {
  auto const bs = base.generic_string();
  auto const ps = p.generic_string();
  if (ps == bs) return true;
  if (ps.size() <= bs.size()) return false;
  return ps.compare(0, bs.size(), bs) == 0 && ps[bs.size()] == '/';
}

// 不切断多字节字符的最大前缀长度（≤ max_bytes）——分页需要精确的字节数做 next_offset
std::size_t utf8_prefix_len(std::string_view s, std::size_t max_bytes) {
  if (s.size() <= max_bytes) return s.size();
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return end;
}

// UTF-8 安全截断（不切断多字节字符，尾部加省略号）—— 与 fetch_url 同款，本地复制避免耦合
std::string utf8_truncate(std::string_view s, std::size_t max_chars) {
  std::size_t const keep = utf8_prefix_len(s, max_chars);
  if (keep == s.size()) return std::string(s);
  return std::string(s.substr(0, keep)) + "…";
}

std::string kPathError(std::string_view p) {
  return "路径越界或非法：「" + std::string(p) +
         "」。只允许沙箱根目录内的**相对**路径，禁止 .. 越界与符号链接逃逸。";
}

}  // namespace

std::optional<std::filesystem::path> resolve_path_in_root(std::filesystem::path const& root,
                                                          std::string_view user_path) {
  if (user_path.find('\0') != std::string_view::npos) return std::nullopt;
  std::string_view const up_view = user_path.empty() ? std::string_view{"."} : user_path;
  std::filesystem::path const up{std::string(up_view)};
  if (up.is_absolute()) return std::nullopt;  // 契约：一律相对 root

  auto const root_abs = std::filesystem::absolute(root).lexically_normal();
  auto target = (root_abs / up).lexically_normal();
  // lexically_normal 对 "root/." 之类保留尾部斜杠；剥掉使根路径/子路径形态一致
  while (target.filename().empty() && target != target.root_path())
    target = target.parent_path();
  if (!is_within(root_abs, target)) return std::nullopt;

  // 符号链接解析后复查：防止 root 内的链接指向 root 之外
  std::error_code ec;
  auto const canon_root = std::filesystem::weakly_canonical(root_abs, ec);
  if (ec) return std::nullopt;
  auto const canon = std::filesystem::weakly_canonical(target, ec);
  if (ec) return std::nullopt;
  if (!is_within(canon_root, canon)) return std::nullopt;

  return target;  // 返回未解析的规范化路径（写操作创建的就是它）
}

ToolSpec make_file_read_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);  // 幂等：沙箱不存在则建

  ToolSpec spec = make_tool(
      "file_read",
      "读取沙箱内的文本文件内容（只读，无副作用）。路径一律**相对**沙箱根目录，"
      "例如读取根目录下 notes/a.txt 传 notes/a.txt。返回 {path, size, offset, next_offset, "
      "content, truncated}。大文件可用 offset+max_chars 分页：next_offset 非空表示后面还有，"
      "把它当下一轮 offset 即可顺序读全（content 末尾的「…」是截断标记，不是文件内容）。"
      "读目录请用 list_dir，找文件位置先 list_dir 或 file_grep。"
      "文件不存在/是目录/二进制 → 报错并说明原因。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的文件路径，如 notes/a.txt")
          .prop("offset", "integer",
                "从文件的第几个字节开始读（≥0，默认 0）；配合 next_offset 顺序读全",
                /*required=*/false)
          .prop("max_chars", "integer",
                "本次返回正文的最大字节数（1–8000，默认 4000）；控制上下文占用",
                /*required=*/false)
          .build(),
      [cfg, root](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        auto path = arg<std::string>(args, "path");
        if (!path) return std::unexpected(path.error());
        auto const target = resolve_path_in_root(root, *path);
        if (!target) return std::unexpected(ToolError{kPathError(*path)});

        long max_chars = cfg.default_read_chars;
        if (auto it = args.find("max_chars"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_read_chars)
            return std::unexpected(ToolError{"argument 'max_chars' invalid: 需为 1–" +
                                             std::to_string(cfg.max_read_chars) + " 的整数"});
          max_chars = it->get<long>();
        }
        long long offset = 0;
        if (auto it = args.find("offset"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long long>() < 0)
            return std::unexpected(
                ToolError{"argument 'offset' invalid: 需为 ≥0 的整数（字节起点）"});
          offset = it->get<long long>();
        }

        std::error_code e2;
        if (!std::filesystem::exists(*target, e2))
          return std::unexpected(ToolError{"文件不存在：「" + *path +
                                           "」。请先用 list_dir 确认路径拼写。"});
        if (std::filesystem::is_directory(*target, e2))
          return std::unexpected(ToolError{"「" + *path +
                                           "」是目录不是文件；读目录请用 list_dir，"
                                           "读文件请输入完整相对路径。"});

        std::ifstream in(*target, std::ios::binary);
        if (!in)
          return std::unexpected(ToolError{"无法打开文件：「" + *path + "」（不存在或权限不足）。"});

        auto const size = std::filesystem::file_size(*target, e2);  // 失败 → 0，仅作展示
        if (static_cast<std::uintmax_t>(offset) > size)
          return std::unexpected(ToolError{"argument 'offset' 超出文件大小（" + std::to_string(size) +
                                           " 字节）：「" + *path + "」"});

        // 多读 4 字节：用于对齐起始字节（offset 可能落在多字节字符中间）并判断"是否还有后文"
        auto const to_read = static_cast<std::uintmax_t>(std::min<std::uintmax_t>(
            size - static_cast<std::uintmax_t>(offset),
            static_cast<std::uintmax_t>(max_chars) + 4));
        in.seekg(static_cast<std::streamoff>(offset));
        std::string data(static_cast<std::size_t>(to_read), '\0');
        in.read(data.data(), static_cast<std::streamsize>(to_read));
        data.resize(static_cast<std::size_t>(in.gcount()));

        if (data.find('\0') != std::string::npos)
          return std::unexpected(ToolError{"检测到二进制内容（NUL 字节），不适合按文本读取；"
                                           "请确认路径或换一个文本文件。"});

        // 起点对齐：offset 落在续字节上时前移到下一个字符边界（保证返回合法 UTF-8）
        std::size_t start = 0;
        while (start < data.size() && (static_cast<unsigned char>(data[start]) & 0xC0) == 0x80)
          ++start;
        data.erase(0, start);
        long long const actual_offset = offset + static_cast<long long>(start);

        std::size_t const kept = utf8_prefix_len(data, static_cast<std::size_t>(max_chars));
        bool const truncated = static_cast<std::uintmax_t>(actual_offset) + kept < size;
        std::string content = data.substr(0, kept);
        if (truncated) content += "…";
        nlohmann::json out{{"path", *path},
                           {"size", static_cast<long long>(size)},
                           {"offset", actual_offset},
                           {"content", std::move(content)},
                           {"truncated", truncated}};
        // 分页：next_offset 非空 = 后面还有内容；把它当下一轮 offset 即可顺序读全
        out["next_offset"] = truncated
                                 ? nlohmann::json(actual_offset + static_cast<long long>(kept))
                                 : nlohmann::json(nullptr);
        return out;
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 文件快照 30 秒后视为可能过期
  return spec;
}

ToolSpec make_file_write_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  ToolSpec spec = make_tool(
      "file_write",
      "向沙箱内的文件写入/追加文本内容（**有副作用**：会创建或修改文件，需审批）。"
      "路径相对沙箱根目录；父目录不存在会自动创建。返回 {path, bytes_written, created, mode}；"
      "created=true 表示新建文件。注意：写入不可逆，重要内容请确认 append 或覆盖是否合适。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的文件路径，如 notes/plan.md")
          .prop("content", "string", "要写入的文本内容（上限 100000 字符）")
          .prop("append", "boolean", "true=追加到文件末尾；false（默认）=覆盖写整个文件",
                /*required=*/false)
          .build(),
      [cfg, root](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        auto path = arg<std::string>(args, "path");
        if (!path) return std::unexpected(path.error());
        auto content = arg<std::string>(args, "content");
        if (!content) return std::unexpected(content.error());
        auto const target = resolve_path_in_root(root, *path);
        if (!target) return std::unexpected(ToolError{kPathError(*path)});

        if (content->size() > cfg.max_write_chars)
          return std::unexpected(ToolError{"内容过长（" + std::to_string(content->size()) +
                                           " 字符，上限 " + std::to_string(cfg.max_write_chars) +
                                           "）。请分多次写入或精简。"});

        bool append = false;
        if (auto a = arg<bool>(args, "append", /*required=*/false); a) append = *a;

        std::error_code e2;
        bool const created = !std::filesystem::exists(*target, e2);
        if (!created && std::filesystem::is_directory(*target, e2))
          return std::unexpected(ToolError{"「" + *path + "」是目录，不能写入。换成文件路径。"});
        std::filesystem::create_directories(target->parent_path(), e2);
        if (e2)
          return std::unexpected(ToolError{"无法创建目标目录：「" + *path + "」：" +
                                           e2.message()});

        std::ofstream out(*target,
                          append ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc));
        if (!out)
          return std::unexpected(ToolError{"无法打开文件用于写入：「" + *path + "」。"});
        out.write(content->data(), static_cast<std::streamsize>(content->size()));
        if (!out)
          return std::unexpected(ToolError{"写入过程中出错：「" + *path + "」（磁盘满或权限？）。"});
        return nlohmann::json{{"path", *path},
                              {"bytes_written", content->size()},
                              {"created", created},
                              {"mode", append ? "append" : "overwrite"}};
      },
      /*read_only=*/false);  // 副作用标记：M7 审批 + SideEffectGuard 防重放

  // wfr-㊷ probe：崩溃缺环（Intent 残留）时自查磁盘现状。
  //   文件不存在            → Absent（未发生，可安全重跑）
  //   内容与本次参数一致    → Complete（已完整写入，不重跑）
  //   存在但内容不一致/短   → Partial（写一半/被改过，fail-closed 请人工）
  //   无法解析路径/读出失败 → Unknown（fail-closed）
  // **只读**：probe 只 stat + 读，绝不写盘（否则 probe 自己成副作用）。
  spec.probe = [cfg, root](nlohmann::json const& args) -> ProbeResult {
    auto path = arg<std::string>(args, "path");
    if (!path) return ProbeResult::Unknown;
    auto const target = resolve_path_in_root(root, *path);
    if (!target) return ProbeResult::Unknown;  // 越界/非法路径：无法可靠探测

    std::string content;
    if (auto c = arg<std::string>(args, "content"); c) content = *c;
    bool append = false;
    if (auto a = arg<bool>(args, "append", /*required=*/false); a) append = *a;

    // append 模式无法凭"内容 = 全文件"判定完成度（写前文件已有内容）→ 报 Unknown fail-closed，
    // 稳妥（宁可请人，不可误判为已完成）。
    if (append) return ProbeResult::Unknown;

    std::error_code ec;
    if (!std::filesystem::exists(*target, ec))
      return ec ? ProbeResult::Unknown : ProbeResult::Absent;

    return probe_file(*target, content, content.size());
  };
  return spec.with_effect_class(EffectClass::Idempotent);  // 治理(problems/33)：确定性写，可安全重放
}

ToolSpec make_list_dir_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  ToolSpec spec = make_tool(
      "list_dir",
      "列出沙箱根目录（或其子目录）下的条目（只读，无副作用）。路径相对沙箱根目录，"
      "缺省列出根目录本身。返回 {path, truncated, entries:[{name, type, size, modified}]}，"
      "按名称排序；type 为 file/dir/symlink。查找文件/确认路径时先列目录再看。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的目录路径，如 notes；缺省为根目录",
                /*required=*/false)
          .prop("max_entries", "integer",
                "最多返回的条目数（1–1000，默认 100），超出则 truncated=true",
                /*required=*/false)
          .build(),
      [cfg, root](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        std::string rpath = ".";
        if (auto p = arg<std::string>(args, "path", /*required=*/false); p && !p->empty())
          rpath = *p;
        auto const target = resolve_path_in_root(root, rpath);
        if (!target) return std::unexpected(ToolError{kPathError(rpath)});

        long max_entries = cfg.default_list_entries;
        if (auto it = args.find("max_entries"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_list_entries)
            return std::unexpected(ToolError{"argument 'max_entries' invalid: 需为 1–" +
                                             std::to_string(cfg.max_list_entries) + " 的整数"});
          max_entries = it->get<long>();
        }

        std::error_code e2;
        if (!std::filesystem::is_directory(*target, e2))
          return std::unexpected(ToolError{"「" + rpath + "」不是目录或不存在；请先用 list_dir 确认。"});
        std::filesystem::directory_iterator it(*target, e2);
        if (e2)
          return std::unexpected(ToolError{"无法读取目录：「" + rpath + "」：" + e2.message()});

        struct Entry {
          std::string name;
          std::string type;
          long long size = 0;
          long long modified = 0;
        };
        std::vector<Entry> entries;
        for (std::filesystem::directory_iterator end; it != end; it.increment(e2)) {
          if (e2) break;
          auto const& de = *it;
          Entry e;
          e.name = de.path().filename().generic_string();
          auto const st = std::filesystem::symlink_status(de, e2);
          if (st.type() == std::filesystem::file_type::symlink) {
            e.type = "symlink";
          } else if (de.is_directory(e2)) {
            e.type = "dir";
          } else if (de.is_regular_file(e2)) {
            e.type = "file";
            e.size = static_cast<long long>(de.file_size(e2));
          } else {
            e.type = "other";
          }
          if (e.type == "file" || e.type == "dir") {
            // 用同一时钟做差 → age，再以系统时钟回推绝对时间：
            // 依赖 file_time_type 的 epoch 与系统一致（有些文件系统/沙箱的 epoch 有偏移）
            auto const ft = de.last_write_time(e2);
            auto const age =
                std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - ft)
                    .count();
            auto const sys_now = std::chrono::system_clock::now();
            e.modified = std::chrono::duration_cast<std::chrono::seconds>(sys_now.time_since_epoch()).count() - age;
          }
          entries.push_back(std::move(e));
        }
        std::sort(entries.begin(), entries.end(),
                  [](Entry const& a, Entry const& b) { return a.name < b.name; });

        bool const truncated = entries.size() > static_cast<std::size_t>(max_entries);
        if (truncated) entries.resize(static_cast<std::size_t>(max_entries));

        auto arr = nlohmann::json::array();
        for (auto const& e : entries) {
          arr.push_back({{"name", e.name},
                         {"type", e.type},
                         {"size", e.size},
                         {"modified", e.modified}});
        }
        return nlohmann::json{{"path", rpath}, {"truncated", truncated}, {"entries", std::move(arr)}};
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 目录快照 30 秒后视为可能过期
  return spec;
}

ToolSpec make_file_grep_tool(FileToolsConfig cfg) {
  auto const root = std::filesystem::absolute(cfg.root_dir);
  std::error_code ec;
  std::filesystem::create_directories(root, ec);

  ToolSpec spec = make_tool(
      "file_grep",
      "在沙箱内按行做正则搜索（只读，无副作用）。路径相对沙箱根目录，缺省搜整个沙箱；"
      "path 可为子目录或单个文件。返回 {pattern, path, matches:[{path, line, text}], "
      "match_count, files_searched, files_skipped, truncated}；line 为 1 起的行号，"
      "text 是命中的整行（超长会截断）。正则语法 ECMAScript；二进制与超大文件会跳过并计入 "
      "files_skipped；运行目录 .wfr/.git 不进搜索面。**没命中 ≠ 内容不存在**——"
      "词面不匹配（同义/变形）时改用其它检索方式，"
      "别据此断言缺失。已知要找的字面/模式时，优先用它定位，再 file_read 只读命中附近。",
      SchemaBuilder()
          .prop("pattern", "string", "正则表达式（ECMAScript 语法），如 \"ERROR|Timeout\"")
          .prop("path", "string", "相对沙箱根目录的子目录或文件路径，缺省 \".\"（整个沙箱）",
                /*required=*/false)
          .prop("ignore_case", "boolean", "true=忽略大小写（默认 false）", /*required=*/false)
          .prop("max_matches", "integer",
                "最多返回的命中行数（1–200，默认 50）；达到上限时 truncated=true",
                /*required=*/false)
          .prop("max_line_chars", "integer",
                "单行展示的最大字节数（1–1000，默认 200），超长行截断后附「…」",
                /*required=*/false)
          .build(),
      [cfg, root](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        (void)ctx;
        auto pattern = arg<std::string>(args, "pattern");
        if (!pattern) return std::unexpected(pattern.error());
        if (pattern->empty())
          return std::unexpected(ToolError{"argument 'pattern' invalid: 不能为空"});

        std::string rpath = ".";
        if (auto p = arg<std::string>(args, "path", /*required=*/false); p && !p->empty())
          rpath = *p;
        auto const target = resolve_path_in_root(root, rpath);
        if (!target) return std::unexpected(ToolError{kPathError(rpath)});

        bool ignore_case = false;
        if (auto it = args.find("ignore_case"); it != args.end() && !it->is_null()) {
          if (!it->is_boolean())
            return std::unexpected(ToolError{"argument 'ignore_case' invalid: 需为布尔值"});
          ignore_case = it->get<bool>();
        }
        long max_matches = cfg.default_grep_matches;
        if (auto it = args.find("max_matches"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_grep_matches)
            return std::unexpected(ToolError{"argument 'max_matches' invalid: 需为 1–" +
                                             std::to_string(cfg.max_grep_matches) + " 的整数"});
          max_matches = it->get<long>();
        }
        long max_line_chars = cfg.default_grep_line_chars;
        if (auto it = args.find("max_line_chars"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<long>() < 1 ||
              it->get<long>() > cfg.max_grep_line_chars)
            return std::unexpected(ToolError{"argument 'max_line_chars' invalid: 需为 1–" +
                                             std::to_string(cfg.max_grep_line_chars) + " 的整数"});
          max_line_chars = it->get<long>();
        }

        std::regex re;
        try {
          auto flags = std::regex_constants::ECMAScript;
          if (ignore_case) flags = flags | std::regex_constants::icase;
          re = std::regex(*pattern, flags);
        } catch (std::regex_error const& e) {
          return std::unexpected(ToolError{"正则表达式无效：「" + *pattern + "」——" + e.what()});
        }

        std::error_code e2;
        if (!std::filesystem::exists(*target, e2))
          return std::unexpected(ToolError{"路径不存在：「" + rpath + "」；请先用 list_dir 确认。"});

        // 收集候选文件（确定顺序：按路径字典序；不跟随符号链接，防逃逸）
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_regular_file(*target, e2)) {
          files.push_back(*target);
        } else if (std::filesystem::is_directory(*target, e2)) {
          for (std::filesystem::recursive_directory_iterator
                   it(*target, std::filesystem::directory_options::skip_permission_denied, e2),
               end;
               it != end; it.increment(e2)) {
            if (e2) { e2.clear(); continue; }  // 权限/竞态错误：跳过该条目，不中断整次搜索
            auto const st = std::filesystem::symlink_status(*it, e2);
            if (st.type() == std::filesystem::file_type::symlink) {
              if (it->is_directory(e2)) it.disable_recursion_pending();  // 目录链接不跟进
              continue;
            }
            // 运行态/版本库目录不进搜索面：.wfr 里是本会话日志与产物（会自己搜到自己），
            // .git 是海量对象（噪声 + 无意义）。需要时可显式传更具体的子路径。
            if (it->is_directory(e2)) {
              std::string const name = it->path().filename().string();
              if (name == ".wfr" || name == ".git") it.disable_recursion_pending();
              continue;
            }
            if (it->is_regular_file(e2)) files.push_back(it->path());
          }
        } else {
          return std::unexpected(
              ToolError{"「" + rpath + "」既不是普通文件也不是目录；请确认路径。"});
        }
        std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) {
          return a.generic_string() < b.generic_string();
        });

        auto arr = nlohmann::json::array();
        long match_count = 0;
        long files_searched = 0;
        long files_skipped = 0;
        std::uintmax_t bytes_scanned = 0;
        bool truncated = false;
        for (auto const& f : files) {
          if (match_count >= max_matches) { truncated = true; break; }
          if (bytes_scanned >= cfg.max_grep_scan_bytes) { truncated = true; break; }  // 总预算封顶
          if (std::filesystem::file_size(f, e2) > cfg.max_grep_file_bytes) {
            ++files_skipped;  // 超大文件：跳过（避免一次搜索吃掉全部预算）
            continue;
          }
          std::ifstream in(f, std::ios::binary);
          if (!in) { ++files_skipped; continue; }
          std::string data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>{});
          if (data.find('\0') != std::string::npos) { ++files_skipped; continue; }  // 二进制
          ++files_searched;
          bytes_scanned += data.size();

          std::string const rel = [&] {
            std::error_code re2;
            auto r = std::filesystem::relative(f, root, re2);
            return re2 ? f.generic_string() : r.generic_string();
          }();
          std::size_t pos = 0;
          long line_no = 0;
          while (pos <= data.size()) {
            std::size_t const nl = data.find('\n', pos);
            std::size_t const endp = nl == std::string::npos ? data.size() : nl;
            std::string line = data.substr(pos, endp - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
            ++line_no;
            bool hit = false;
            try {
              hit = std::regex_search(line, re);
            } catch (std::regex_error const& ex) {
              // 复杂模式可能在匹配期抛（如复杂度上限）；一次搜索失败不该炸整个工具
              return std::unexpected(ToolError{"正则匹配失败：「" + *pattern + "」——" + ex.what()});
            }
            if (hit) {
              arr.push_back({{"path", rel},
                             {"line", line_no},
                             {"text", utf8_truncate(line, static_cast<std::size_t>(max_line_chars))}});
              ++match_count;
              if (match_count >= max_matches) { truncated = true; break; }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
          }
        }

        return nlohmann::json{{"pattern", *pattern},
                              {"path", rpath},
                              {"matches", std::move(arr)},
                              {"match_count", match_count},
                              {"files_searched", files_searched},
                              {"files_skipped", files_skipped},
                              {"bytes_scanned", bytes_scanned},
                              {"truncated", truncated}};
      },
      /*read_only=*/true);
  spec.ttl = cfg.ttl;  // 文件内容快照 30 秒后视为可能过期
  return spec;
}

std::vector<ToolSpec> make_file_tools(FileToolsConfig cfg) {
  std::vector<ToolSpec> out;
  out.push_back(make_file_read_tool(cfg));
  out.push_back(make_file_write_tool(cfg));
  out.push_back(make_list_dir_tool(cfg));
  out.push_back(make_file_grep_tool(cfg));
  out.push_back(make_file_extract_tool(cfg));
  out.push_back(make_file_sample_tool(cfg));
  return out;
}

}  // namespace cppchain::tool