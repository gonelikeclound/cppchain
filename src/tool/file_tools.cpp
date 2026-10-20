// cppchain —— src/tool/file_tools.cpp
#include "cppchain/tool/file_tools.hpp"

#include <algorithm>
#include <chrono>
#include <expected>
#include <fstream>
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

// UTF-8 安全截断（不切断多字节字符，尾部加省略号）—— 与 fetch_url 同款，本地复制避免耦合
std::string utf8_truncate(std::string_view s, std::size_t max_chars) {
  if (s.size() <= max_chars) return std::string(s);
  std::size_t end = max_chars;
  while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
  return std::string(s.substr(0, end)) + "…";
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
      "例如读取根目录下 notes/a.txt 传 notes/a.txt。返回 {path, size, content, truncated}；"
      "大文件会按 max_chars 截断并在 truncated 标出。读目录请用 list_dir，"
      "找文件位置先 list_dir 再看。文件不存在/是目录/二进制 → 报错并说明原因。",
      SchemaBuilder()
          .prop("path", "string", "相对沙箱根目录的文件路径，如 notes/a.txt")
          .prop("max_chars", "integer",
                "返回正文的最大字符数（1–8000，默认 4000）；控制上下文占用",
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
        long const to_read = static_cast<long>(
            std::min<std::uintmax_t>(size, static_cast<std::uintmax_t>(max_chars + 1)));
        std::string data(static_cast<std::size_t>(to_read), '\0');
        in.read(data.data(), to_read);
        data.resize(static_cast<std::size_t>(in.gcount()));

        if (data.find('\0') != std::string::npos)
          return std::unexpected(ToolError{"检测到二进制内容（NUL 字节），不适合按文本读取；"
                                           "请确认路径或换一个文本文件。"});
        bool const truncated = data.size() > static_cast<std::size_t>(max_chars);
        if (truncated) data = utf8_truncate(data, static_cast<std::size_t>(max_chars));
        return nlohmann::json{{"path", *path},
                              {"size", static_cast<long long>(size)},
                              {"content", std::move(data)},
                              {"truncated", truncated}};
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

std::vector<ToolSpec> make_file_tools(FileToolsConfig cfg) {
  std::vector<ToolSpec> out;
  out.push_back(make_file_read_tool(cfg));
  out.push_back(make_file_write_tool(cfg));
  out.push_back(make_list_dir_tool(cfg));
  return out;
}

}  // namespace cppchain::tool