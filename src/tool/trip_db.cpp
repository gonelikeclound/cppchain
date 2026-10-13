// cppchain —— src/tool/trip_db.cpp
// B2-B 出行规划业务 DB 工具面（确定性种子集 + 自动验证器）。数据刻意做小，
// 保证「搜索组合空间有限、验证候选易」；验证规则照 DeepSeek-V3.2 §3.2.3：
// 不重复城市/酒店/餐厅/景点/餐厅；每项在当日所住城市；第二日预算档约束。
#include "cppchain/tool/trip_db.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace cppchain::tool {
namespace {

using Json = nlohmann::json;
using Res = std::expected<Json, ToolError>;

Json city(std::string const& name, std::string const& weather) {
  return Json{{"name", name}, {"weather", weather}};
}
Json hotel(std::string name, std::string c, double night, double star) {
  return Json{{"name", std::move(name)}, {"city", std::move(c)}, {"night_cny", night},
              {"star", star}, {"id", name}};
}
Json rest(std::string name, std::string c, double star, std::string cuisine) {
  return Json{{"name", std::move(name)}, {"city", std::move(c)}, {"star", star},
              {"cuisine", std::move(cuisine)}, {"id", name}};
}
Json attr(std::string name, std::string c, double ticket) {
  return Json{{"name", std::move(name)}, {"city", std::move(c)}, {"ticket_cny", ticket},
              {"id", name}};
}

struct DB {
  std::vector<Json> cities = {city("北京", "晴"),  city("杭州", "多云"), city("上海", "雨"),
                              city("南京", "阴"),  city("苏州", "晴"),  city("广州", "晴")};
  std::vector<Json> hotels = {
      hotel("杭州西湖大酒店", "杭州", 1200, 4.7), hotel("杭州钱江朗廷", "杭州", 650, 4.5),
      hotel("杭州余杭智选", "杭州", 320, 3.6),   hotel("杭州云栖民宿", "杭州", 880, 4.2),
      hotel("上海外滩W", "上海", 1500, 4.9),     hotel("上海静安高登", "上海", 700, 4.3),
      hotel("上海虹桥如家", "上海", 280, 3.5),   hotel("北京王府井希尔顿", "北京", 1300, 4.8),
      hotel("北京中关村全季", "北京", 480, 4.0), hotel("北京故宫民宿", "北京", 360, 3.6),
      hotel("苏州金鸡湖凯宾斯基", "苏州", 1100, 4.6), hotel("苏州观前汉庭", "苏州", 260, 3.3),
      hotel("南京玄武湖绿地", "南京", 900, 4.4), hotel("南京夫子庙速八", "南京", 220, 3.1),
      hotel("广州小蛮腰万豪", "广州", 1250, 4.7), hotel("广州珠江新城亚朵", "广州", 540, 4.1)};
  std::vector<Json> restaurants = {
      rest("杭州楼外楼", "杭州", 4.3, "浙菜"), rest("杭州外婆家", "杭州", 3.4, "家常"),
      rest("杭州绿茶", "杭州", 3.1, "杭帮菜"), rest("杭州知味观", "杭州", 4.0, "小笼"),
      rest("上海小南国", "上海", 4.1, "本帮"), rest("上海生煎铺", "上海", 3.6, "小吃"),
      rest("北京全聚德", "北京", 4.3, "烤鸭"), rest("北京胡同面馆", "北京", 2.9, "面食"),
      rest("苏州藏书羊肉馆", "苏州", 4.2, "羊肉"), rest("南京盐水鸭店", "南京", 3.3, "鸭"),
      rest("广州早茶轩", "广州", 4.4, "茶点")};
  std::vector<Json> attractions = {
      attr("西湖", "杭州", 0), attr("灵隐寺", "杭州", 75), attr("宋城", "杭州", 350),
      attr("外滩", "上海", 0), attr("东方明珠", "上海", 220), attr("上海迪士尼", "上海", 529),
      attr("故宫", "北京", 60), attr("颐和园", "北京", 30), attr("环球影城", "北京", 700),
      attr("拙政园", "苏州", 90), attr("金鸡湖", "苏州", 0), attr("南京大屠杀馆", "南京", 0),
      attr("夫子庙", "南京", 0), attr("珠江夜游", "广州", 120), attr("小蛮腰", "广州", 150)};
  // city → 天气（按日期生成：seed 简单稳定）
  Json weather_of(std::string const& c, std::string const& d) const {
    for (auto const& ci : cities) if (ci["name"] == c) return ci["weather"];
    return Json("未知");
  }
  Json by_city(std::vector<Json> const& v, std::string const& c) const {
    Json arr = Json::array();
    for (auto const& x : v) if (x["city"].get<std::string>() == c) arr.push_back(x);
    return arr;
  }
};

// 按提交 JSON 逐约束验证，返回 {ok, issues}；并写 .out/trip_verdict.json
Json validate_submit(Json const& plan) {
  DB db;
  std::vector<std::string> issues;
  std::vector<std::string> seen_city, seen_hotel, seen_rest, seen_attr;
  bool day2_budget_ok = true;
  double d2_hotel = 0, d2_lunch = 0, d2_dinner = 0;
  double d2_lunch_star = 0, d2_dinner_star = 0, d2_attr = 0;
  auto find = [&db](auto const& v, std::string const& name) {
    for (auto const& x : v) if (x["name"] == name) return x;
    return Json::object();
  };
  if (!plan.is_array()) issues.push_back("submit 应为数组");
  auto add = [&issues](std::string s) { issues.push_back(std::move(s)); };
  for (auto const& day : plan) {
    if (!day.is_object()) continue;
    std::string const c = day.value("city", "");
    if (c.empty()) add("某天缺 city");
    if (!c.empty()) { if (std::find(seen_city.begin(), seen_city.end(), c) != seen_city.end()) add("城市重复: " + c); seen_city.push_back(c); }
    auto const h = find(db.hotels, day.value("hotel", ""));
    auto const rl = find(db.restaurants, day.value("afternoon_restaurant", ""));
    auto const rd = find(db.restaurants, day.value("evening_restaurant", ""));
    auto const at = find(db.attractions, day.value("afternoon_attraction", ""));
    auto each = [&](std::string const& name, Json const& x, char kind, std::vector<std::string>& seen) {
      if (x.empty()) { add("查无 " + name); return; }
      if (x["city"].get<std::string>() != c) add(name + " 不在当日城市 " + c);
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) add(kind + std::string("重复: ") + name);
      seen.push_back(name);
    };
    each(day.value("hotel", ""), h, 'H', seen_hotel);
    each(day.value("afternoon_restaurant", ""), rl, 'R', seen_rest);
    each(day.value("evening_restaurant", ""), rd, 'R', seen_rest);
    each(day.value("afternoon_attraction", ""), at, 'A', seen_attr);
  }
  // 第二日预算档（以第 2 天计）
  if (plan.size() >= 2 && plan[1].is_object()) {
    auto const h2 = find(db.hotels, plan[1].value("hotel", ""));
    if (!h2.empty()) {
      d2_hotel = h2.value("night_cny", 0.0);
      d2_lunch_star = find(db.restaurants, plan[1].value("afternoon_restaurant", "")).value("star", 0.0);
      d2_dinner_star = find(db.restaurants, plan[1].value("evening_restaurant", "")).value("star", 0.0);
      d2_attr = find(db.attractions, plan[1].value("afternoon_attraction", "")).value("ticket_cny", 0.0);
    }
    Json ok_branch = Json::object();
    if (d2_hotel >= 800) {
      day2_budget_ok = (d2_lunch_star >= 4.0 && d2_dinner_star >= 4.0 && d2_attr > 0 && d2_attr < 120);
      if (!day2_budget_ok) add("次日豪华档约束（两餐≥4.0星、门票<120）未满足");
    } else if (d2_hotel >= 500) {
      day2_budget_ok = (d2_lunch_star >= 4.0 || d2_dinner_star >= 4.0) && d2_attr < 180;
      if (!day2_budget_ok) add("次日中高档约束（一餐≥4.0、门票<180）未满足");
    } else {
      day2_budget_ok = (d2_lunch_star >= 3.2 || d2_dinner_star >= 3.2);
      if (!day2_budget_ok) add("次日经济档约束（一餐≥3.2）未满足");
    }
  }
  bool const ok = issues.empty();
  Json verdict{{"verdict", ok ? "pass" : "fail"}, {"issues", issues}};
  std::error_code ec;
  std::filesystem::create_directories(".out", ec);
  std::ofstream f(".out/trip_verdict.json", std::ios::trunc);
  if (f) f << verdict.dump();
  return verdict;
}

Res require(Json const& args, std::string_view key) {
  auto it = args.find(key);
  if (it == args.end()) return std::unexpected(ToolError{"缺参数: " + std::string(key)});
  return Res(*it);
}

}  // namespace

std::vector<ToolSpec> make_trip_tools() {
  std::vector<ToolSpec> out;
  // 任务必需的最小查询面（确定性种子集）+ submit_result
  auto mk = [](char const* name, std::string const& d, std::function<Json(Json const&)> f) {
    return make_tool(name, d, nlohmann::json::object(),
                     [f = std::move(f)](Json const& args, ToolContext&) -> Res { return Res(f(args)); },
                     /*read_only=*/true);
  };
  out.push_back(mk("get_all_cities", "返回所有城市", [](Json const&) {
    DB d; return d.cities; }));
  out.push_back(mk("get_all_hotels_by_city", "返回某城的酒店（含 night_cny/star）", [](Json const& a) {
    DB d; return d.by_city(d.hotels, a.value("city", "")); }));
  out.push_back(mk("get_all_restaurants_by_city", "返回某城的餐厅（含 star）", [](Json const& a) {
    DB d; return d.by_city(d.restaurants, a.value("city", "")); }));
  out.push_back(mk("get_all_attractions_by_city", "返回某城的景点（含 ticket_cny）", [](Json const& a) {
    DB d; return d.by_city(d.attractions, a.value("city", "")); }));
  out.push_back(mk("get_weather_by_city_date", "返回某城某日天气", [](Json const& a) {
    DB d; return d.weather_of(a.value("city", ""), a.value("date", "")); }));
  // submit_result（写 .out/trip_verdict.json 供 L2 判据）：参数 = plan 数组 JSON
  Json props = Json{{"plan", Json{{"type", "string"}, {"description", "三日行程 JSON（数组，每天含 time/city/hotel/afternoon_restaurant/afternoon_attraction/evening_restaurant）"}}}};
  ToolSpec sub = make_tool("submit_result", "提交最终行程并自动校验（写 .out/trip_verdict.json）",
                           props,
                           [](Json const& args, ToolContext&) -> Res {
                             auto p = arg<std::string>(args, "plan");
                             if (!p) return std::unexpected(ToolError{"缺 plan"});
                             auto j = nlohmann::json::parse(*p, nullptr, /*allow_exceptions=*/false);
                             if (j.is_discarded()) return std::unexpected(ToolError{"plan 不是合法 JSON"});
                             return Res(validate_submit(j));
                           },
                           /*read_only=*/false);
  out.push_back(std::move(sub));
  return out;
}

}  // namespace cppchain::tool