// cppchain —— src/tool/weather.cpp
// 真实天气查询工具（Open-Meteo，免费无密钥）：
//   ① 地名 → 候选坐标（geocoding，中/英文均查，按人口+国家消歧）
//   ② 坐标 → 当前天气 + 逐日预报（forecast）
// 失败一律回退为 ToolError，让模型自我修正（补国家/换写法）而不是编造天气。
#include "cppchain/tool/weather.hpp"

#include <algorithm>
#include <cmath>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cppchain/tool/http_json.hpp"

namespace cppchain::tool {

namespace {

constexpr std::string_view kGeocodePath = "/v1/search";
constexpr std::string_view kForecastPath = "/v1/forecast";

std::string join_url(std::string base, std::string_view path) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + std::string(path);
}

// 计算一位：double 的确定性格式化用 std::format（C++26）。
std::string f1(double v) { return std::format("{:.1f}", v); }

std::string wind_dir(double deg) {
  // 8 方位：正北为 0°，顺时针
  constexpr std::array<std::string_view, 8> kDirs{
      "北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风"};
  int idx = static_cast<int>(std::lround((deg + 360) / 45.0)) % 8;
  return std::string(kDirs[idx]);
}

// ---- 地名消歧候选 ----
struct Candidate {
  double lat = std::numeric_limits<double>::quiet_NaN();
  double lon = std::numeric_limits<double>::quiet_NaN();
  std::string name;
  std::string country_code;
  std::string country;
  std::string admin1;
  long long population = 0;
  std::string feature_code;
};

// Open-Meteo geocoding 对中文名的匹配并不完美（「纽约」要写成 New York），
// 所以这里按 population/feature_code 排序而不是盲取第一条；country 参数可进一步消歧。
// （详见 04-problems —— 状态时效性/工具返回鲁棒性的观测点）
bool is_populated(std::string_view fc) { return !fc.empty() && fc.front() == 'P'; }

void collect_candidates(nlohmann::json const& body, std::vector<Candidate>& out) {
  if (!body.is_object()) return;
  auto it = body.find("results");
  if (it == body.end() || !it->is_array()) return;
  for (auto const& r : *it) {
    if (!r.is_object()) continue;
    Candidate c;
    auto lat = r.find("latitude"), lon = r.find("longitude");
    if (lat == r.end() || lon == r.end() || !lat->is_number() || !lon->is_number())
      continue;
    c.lat = lat->get<double>();
    c.lon = lon->get<double>();
    c.name = r.value("name", "");
    c.country_code = r.value("country_code", "");
    c.country = r.value("country", "");
    c.admin1 = r.value("admin1", "");
    c.feature_code = r.value("feature_code", "");
    if (auto p = r.find("population"); p != r.end() && p->is_number())
      c.population = p->get<long long>();
    out.push_back(std::move(c));
  }
}

// 按经纬度去重（同一地点中文/英文两轮可能各返回一条）
void dedupe(std::vector<Candidate>& v) {
  std::stable_sort(v.begin(), v.end(), [](Candidate const& a, Candidate const& b) {
    if (std::round(a.lat * 1e4) != std::round(b.lat * 1e4)) return a.lat < b.lat;
    return a.lon < b.lon;
  });
  v.erase(std::unique(v.begin(), v.end(), [](Candidate const& a, Candidate const& b) {
            return std::round(a.lat * 1e4) == std::round(b.lat * 1e4) &&
                   std::round(a.lon * 1e4) == std::round(b.lon * 1e4);
          }),
          v.end());
}

// 排序优先级：国家匹配 > 居民点(feature_code P*) > 人口多；人口未知按 0。
std::optional<Candidate> pick_best(std::vector<Candidate> const& v,
                                   std::string const& country) {
  if (v.empty()) return std::nullopt;
  std::vector<Candidate> pool = v;
  if (!country.empty())
    pool.erase(std::remove_if(pool.begin(), pool.end(),
                              [&](Candidate const& c) {
                                return !c.country_code.empty() &&
                                       c.country_code != country;
                              }),
               pool.end());
  if (pool.empty()) pool = v;  // 国家过滤过头 → 退回全量

  auto const best = std::max_element(pool.begin(), pool.end(), [](Candidate const& a, Candidate const& b) {
    bool const ap = is_populated(a.feature_code), bp = is_populated(b.feature_code);
    if (ap != bp) return !ap;  // P* 优先
    return a.population < b.population;
  });
  return *best;
}

// 常见地名后缀：查询无果时剥掉再试（「上海市」→「上海」）
std::string strip_region_suffix(std::string s) {
  for (std::string_view suf : {"市", "省", "州", "縣", "县", "郡"}) {
    if (s.size() > suf.size() && s.ends_with(suf)) return s.substr(0, s.size() - suf.size());
  }
  return {};
}

}  // namespace

std::string_view weather_code_name(int code) {
  switch (code) {
    case 0: return "晴";
    case 1: return "基本晴";
    case 2: return "局部多云";
    case 3: return "阴";
    case 45: return "有雾";
    case 48: return "冻雾";
    case 51: return "轻毛毛雨";
    case 53: return "毛毛雨";
    case 55: return "浓毛毛雨";
    case 56: return "轻冻毛毛雨";
    case 57: return "浓冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: return "轻冻雨";
    case 67: return "重冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "雪粒";
    case 80: return "小阵雨";
    case 81: return "中阵雨";
    case 82: return "强阵雨";
    case 85: return "小阵雪";
    case 86: return "强阵雪";
    case 95: return "雷暴";
    case 96: return "雷暴伴小冰雹";
    case 99: return "雷暴伴大冰雹";
    default: return "未知";
  }
}

namespace {

// 一次 geocoding 查询（中文或英文），返回候选列表
std::expected<std::vector<Candidate>, ToolError> query_geocode(
    http::Transport& transport, WeatherToolConfig const& cfg, std::string const& name,
    std::string_view language, core::CancelToken const& cancel) {
  http::HttpRequest req;
  req.url = join_url(cfg.geocode_base, kGeocodePath) +
            "?name=" + url_percent_encode(name) +
            "&count=10&language=" + std::string(language) + "&format=json";
  req.method = "GET";  // Open-Meteo 只接受 GET 查询
  req.timeout_ms = cfg.timeout_ms;
  req.ca_info = cfg.ca_info;

  auto r = http_json(transport, req, cancel);
  if (!r) return std::unexpected(r.error());
  if (auto e = http_status_error(r->status, "地名解析服务", r->raw))
    return std::unexpected(*e);
  std::vector<Candidate> out;
  collect_candidates(r->body, out);
  return out;
}

}  // namespace

ToolSpec make_weather_tool(std::shared_ptr<http::Transport> transport,
                           WeatherToolConfig cfg) {
  ToolSpec spec = make_tool(
      "get_weather",
      "查询指定城市/地点的**实时天气**与未来几天逐日预报。城市名支持中文或英文；"
      "同名地点容易混淆（如「上海市」也可能匹配美国伊利诺伊州的一个小镇），"
      "遇到这种情况请附带 country（ISO 3166-1 alpha-2）或改用当地官方拼写（如 New York、Tokyo）。"
      "数据来自 Open-Meteo 免费公开接口，返回结构化 JSON。",
      SchemaBuilder()
          .prop("city", "string", "城市或地名（中文/英文，如「北京」「beijing」）")
          .prop("country", "string",
                "国家代码（ISO 3166-1 alpha-2，如 CN/US），同各地名消歧用；可省略",
                /*required=*/false)
          .prop("days", "integer", "预报天数：1–7，默认 1", /*required=*/false)
          .build(),
      [transport, cfg](nlohmann::json const& args, ToolContext& ctx)
          -> std::expected<nlohmann::json, ToolError> {
        auto city = arg<std::string>(args, "city");
        if (!city) return std::unexpected(city.error());

        std::string country;
        if (auto c = arg<std::string>(args, "country", /*required=*/false); c)
          country = *c;

        int days = 1;
        if (auto it = args.find("days"); it != args.end() && !it->is_null()) {
          if (!it->is_number_integer() || it->get<int>() < 1 || it->get<int>() > 7)
            return std::unexpected(ToolError{"argument 'days' invalid: 需为 1–7 的整数"});
          days = it->get<int>();
        }

        // ---- ① 地名 → 坐标（zh 一轮；无居民点再补 en；带后缀剥后缀再试）----
        std::vector<Candidate> cands;
        auto const has_populated = [&] {
          return !cands.empty() && is_populated(cands.front().feature_code);
        };

        // ① zh 一轮；② 无居民点补 en；③ 带后缀剥后缀再试。
        // geocoding 的传输/HTTP 错误直接上抛（区别于"查无此地"——后者才让模型换写法）。
        auto r1 = query_geocode(*transport, cfg, *city, "zh", ctx.cancel);
        if (!r1) return std::unexpected(r1.error());
        cands = std::move(*r1);

        if (!has_populated()) {
          auto r2 = query_geocode(*transport, cfg, *city, "en", ctx.cancel);
          if (!r2) return std::unexpected(r2.error());
          if (!r2->empty()) cands.insert(cands.end(), r2->begin(), r2->end());
        }
        if (!has_populated()) {
          if (auto stem = strip_region_suffix(*city); !stem.empty()) {
            auto r3 = query_geocode(*transport, cfg, stem, "zh", ctx.cancel);
            if (!r3) return std::unexpected(r3.error());
            cands.insert(cands.end(), r3->begin(), r3->end());
          }
        }
        dedupe(cands);
        auto best = pick_best(cands, country);
        if (!best)
          return std::unexpected(ToolError{
              "找不到城市「" + *city +
              "」。请确认地名写法（中文「北京」或英文「Beijing」均可）；"
              "同名时请带 country 参数区分，如 get_weather(city=上海市, country=CN)。"});

        // ---- ② 坐标 → 天气 ----
        http::HttpRequest f;
        f.url = join_url(cfg.forecast_base, kForecastPath) + "?latitude=" + f1(best->lat) +
                "&longitude=" + f1(best->lon) +
                "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
                "weather_code,wind_speed_10m,wind_direction_10m,precipitation" +
                "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum" +
                "&timezone=auto&forecast_days=" + std::to_string(days);
        f.method = "GET";  // Open-Meteo 只接受 GET 查询
        f.timeout_ms = cfg.timeout_ms;
        f.ca_info = cfg.ca_info;

        auto fr = http_json(*transport, f, ctx.cancel);
        if (!fr) return std::unexpected(fr.error());
        if (auto e = http_status_error(fr->status, "天气服务", fr->raw))
          return std::unexpected(*e);
        auto const& fc = fr->body;
        if (!fc.is_object())
          return std::unexpected(ToolError{"天气服务返回了无法解析的内容"});

        // ---- ③ 组装结构化结果（含人类可读 summary）----
        nlohmann::json current = nlohmann::json::object();
        if (auto c = fc.find("current"); c != fc.end() && c->is_object()) {
          double t = c->value("temperature_2m", std::numeric_limits<double>::quiet_NaN());
          double feels = c->value("apparent_temperature", t);
          int code = c->value("weather_code", -1);
          current = {
              {"time", c->value("time", "")},
              {"weather", std::string(weather_code_name(code))},
              {"weather_code", code},
              {"temperature_c", t},
              {"feels_like_c", feels},
              {"humidity_pct", c->value("relative_humidity_2m", -1)},
              {"wind_speed_kmh", c->value("wind_speed_10m", -1)},
              {"wind_direction_deg", c->value("wind_direction_10m", -1)},
              {"precipitation_mm", c->value("precipitation", -1)},
          };

          std::string loc = best->name.empty() ? *city : best->name;
          if (!best->admin1.empty() && best->admin1 != best->name) loc += "（" + best->admin1 + "）";
          std::string sum = loc + " " + std::string(weather_code_name(code));
          sum += " " + f1(t) + "°C";
          if (t == t && feels == feels) sum += "（体感 " + f1(feels) + "°C）";
          double h = c->value("relative_humidity_2m", std::numeric_limits<double>::quiet_NaN());
          if (h == h) sum += "，湿度 " + f1(h) + "%";
          double w = c->value("wind_speed_10m", std::numeric_limits<double>::quiet_NaN());
          double wd = c->value("wind_direction_10m", std::numeric_limits<double>::quiet_NaN());
          if (w == w) sum += "，" + (wd == wd ? wind_dir(wd) : "风") + " " + f1(w) + " km/h";
          if (auto t0 = current.find("time"); t0 != current.end())
            sum += "（当地时间 " + t0->get<std::string>() + "）";
          current["summary"] = sum;
        }

        nlohmann::json daily = nlohmann::json::array();
        if (auto d = fc.find("daily"); d != fc.end() && d->is_object()) {
          auto const& day = *d;
          auto times = day.value("time", nlohmann::json::array());
          auto tmax = day.value("temperature_2m_max", nlohmann::json::array());
          auto tmin = day.value("temperature_2m_min", nlohmann::json::array());
          auto codes = day.value("weather_code", nlohmann::json::array());
          for (std::size_t i = 0; i < std::min<std::size_t>(times.size(), days); ++i) {
            int code = codes.is_array() && i < codes.size() ? codes[i].get<int>() : -1;
            daily.push_back({
                {"date", times[i]},
                {"tmax_c", tmax.is_array() && i < tmax.size() ? tmax[i] : nlohmann::json(nullptr)},
                {"tmin_c", tmin.is_array() && i < tmin.size() ? tmin[i] : nlohmann::json(nullptr)},
                {"weather", std::string(weather_code_name(code))},
            });
          }
        }

        nlohmann::json location = {{"name", best->name.empty() ? *city : best->name},
                                   {"country", best->country},
                                   {"country_code", best->country_code},
                                   {"admin1", best->admin1},
                                   {"latitude", best->lat},
                                   {"longitude", best->lon}};

        return nlohmann::json{
            {"summary", current.value("summary", f1(best->lat) + "," + f1(best->lon))},
            {"location", std::move(location)},
            {"current", std::move(current)},
            {"daily", std::move(daily)},
            {"source", "https://open-meteo.com"},
        };
      },
      /*read_only=*/true);
  spec.category = "retrieval";  // 供历史精简按类别过滤（wfr-㊵）
  spec.ttl = cfg.ttl;  // 天气快照 30 分钟后视为可能过期（状态时效性，见 04-problems/06）
  return spec;
}

}  // namespace cppchain::tool