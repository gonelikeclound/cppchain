// cppchain —— src/workflow/generator.cpp
#include "cppchain/workflow/generator.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

#include "cppchain/message/message.hpp"
#include "cppchain/model/chat_model.hpp"
#include "cppchain/model/error.hpp"
#include "cppchain/model/options.hpp"
#include "cppchain/state/store.hpp"
#include "cppchain/workflow/validator.hpp"

namespace cppchain::workflow {

namespace {

using cppchain::message::Message;
using cppchain::message::human_message;
using cppchain::message::system_message;

// ---- 工具函数 ----

// 从模型输出里抽出一个 JSON 对象：整体解析优先，失败则取首尾花括号之间子串
//（容忍 ```json 围栏）。解析不出 → nullopt。
std::optional<Json> extract_json(std::string const& s) {
  try {
    Json j = Json::parse(s);
    if (j.is_object()) return j;
  } catch (nlohmann::json::exception const&) {
  }
  std::size_t const first = s.find('{');
  std::size_t const last = s.rfind('}');
  if (first == std::string::npos || last == std::string::npos || last <= first)
    return std::nullopt;
  try {
    Json j = Json::parse(s.substr(first, last - first + 1));
    if (j.is_object()) return j;
  } catch (nlohmann::json::exception const&) {
  }
  return std::nullopt;
}

// 工具目录行（供提示词）：name(参数…) —— 描述（副作用标注）
std::string tool_catalog(tool::ToolRegistry const& registry) {
  std::string out;
  for (tool::ToolSpec const& t : registry.all()) {
    std::string params;
    if (t.parameters.contains("properties") && t.parameters["properties"].is_object())
      for (auto it = t.parameters["properties"].begin();
           it != t.parameters["properties"].end(); ++it) {
        if (!params.empty()) params += ", ";
        params += it.key();
      }
    out += "· " + t.name + "(" + params + ") —— " + t.description +
           (t.read_only ? "（只读）" : "（有副作用）") + "\n";
  }
  return out;
}

inline const std::vector<std::string> kSessionNs = {"wf", "session"};

Json questions_to_json(std::vector<ClarifyQuestion> const& qs) {
  Json arr = Json::array();
  for (ClarifyQuestion const& q : qs) {
    Json j{{"id", q.id}, {"text", q.text}};
    if (!q.options.empty()) j["options"] = q.options;
    arr.push_back(std::move(j));
  }
  return arr;
}

std::vector<ClarifyQuestion> questions_from_json(Json const& arr) {
  std::vector<ClarifyQuestion> out;
  if (!arr.is_array()) return out;
  for (Json const& e : arr) {
    ClarifyQuestion q;
    q.id = e.value("id", "");
    q.text = e.value("text", e.value("ask", ""));
    if (!q.id.empty()) {
      for (Json const& o : e.value("options", Json::array()))
        if (o.is_string()) q.options.push_back(o.get<std::string>());
      out.push_back(std::move(q));
    }
  }
  return out;
}

Json qa_to_json(std::vector<std::pair<std::string, std::string>> const& qa) {
  Json arr = Json::array();
  for (auto const& [q, a] : qa) arr.push_back(Json::array({q, a}));
  return arr;
}

std::vector<std::pair<std::string, std::string>> qa_from_json(Json const& arr) {
  std::vector<std::pair<std::string, std::string>> out;
  if (!arr.is_array()) return out;
  for (Json const& e : arr)
    if (e.is_array() && e.size() == 2) out.emplace_back(e[0].get<std::string>(),
                                                        e[1].get<std::string>());
  return out;
}

}  // namespace

struct WorkflowGenerator::Impl {
  model::ChatModel* model = nullptr;
  tool::ToolRegistry const* registry = nullptr;
  state::Store* external_store = nullptr;
  std::unique_ptr<state::MemoryStore> owned_store;  // external_store 为空时自持
  GeneratorOptions opts;
  int calls = 0;
  long counter = 0;  // 会话号
  UsageReport usage;
  std::string last_raw;

  state::Store& store() {
    if (external_store != nullptr) return *external_store;
    if (owned_store == nullptr) owned_store = std::make_unique<state::MemoryStore>();
    return *owned_store;
  }

  // ---- 一次模型调用：预算前置检查 + usage 记账 + 原样留底 ----
  std::optional<Message> call(std::vector<Message> const& msgs, bool is_plan,
                              std::string& error_out) {
    if (calls >= opts.model_calls_budget) {
      error_out = "模型调用预算耗尽（" + std::to_string(opts.model_calls_budget) + " 次）";
      return std::nullopt;
    }
    ++calls;
    auto r = model->invoke(msgs, cppchain::model::CallOptions{});
    if (!r) {
      error_out = "模型调用失败：" + std::string(cppchain::model::kind_str(r.error().kind)) +
                  " " + r.error().message;
      return std::nullopt;
    }
    if (r->usage) {
      if (is_plan) {
        usage.plan_in += r->usage->input_tokens;
        usage.plan_out += r->usage->output_tokens;
        ++usage.plan_calls;
      } else {
        usage.ir_in += r->usage->input_tokens;
        usage.ir_out += r->usage->output_tokens;
        ++usage.ir_calls;
      }
    }
    last_raw = r->text();
    return *r;
  }

  // ---- 提示词 ----

  std::string system_prompt() const {
    if (!opts.system_prompt.empty()) return opts.system_prompt;
    return
        "你是主动智能 Workflow 生成器：把用户意图转成可执行 Workflow 的规划（plan）。\n"
        "硬规则：\n"
        "· trigger **可省略**：省略表示『由宿主手工触发』（wfr 直接拉起），不做调度唤醒。"
        "给出时只表达『何时醒』：只能 time 源 + schedule 形态 + 5 字段 cron"
        "（分 时 日 月 周，如 \"0 8,20 * * *\"）。业务条件（价格高低/库存有无/结果非空）"
        "绝不进 trigger，一律下沉为一个 expr 判断节点。\n"
        "· 节点：tool（调目录工具，成功出口 next）、expr（条件 then/else）、"
        "label（循环目标）、goto（有界回跳 max 正整数）。流程连成单链、无死节点。\n"
        "· 参数引用：$_.result.<字段>（最近一步工具输出）/ $<节点id>.result.<字段> / "
        "@config.<键>；只能引用更早节点 output_schema 声明的字段。\n"
        "· 诚实性：目录里没有『下单/购买』类工具时，绝不允许写成『已自动下单』，"
        "应降级为『提醒用户确认下单』。\n"
        "· 缺执行能力（动作无法自动化，如目录里没有下单工具）→ **应当降级**『少做』"
        "（降级为提醒用户确认），不得据此 reject —— 与上面的诚实性规则同一处理。\n"
        "· 缺表达力（意图核心是『被事件唤醒』这类触发语义，而 trigger 只有 time 源）"
        "→ 必须显式 reject 并说明缺口。注意『结果非空』『价格高低』属于业务条件，"
        "**不是**表达力缺口：它们能写成 expr 判断（!= null 判取没取到、"
        "!= \"\" 判字符串非空），下沉为判断节点即可。\n"
        "可用工具目录：\n" +
        tool_catalog(*registry) +
        "\n一次调用只输出一个 JSON 对象，三态只选一：\n"
        "  计划就绪   {\"plan\": {\"trigger\": {...}, \"steps\": [...], \"assumptions\": [...]}}\n"
        "  需澄清     {\"questions\": [{\"id\": \"q1\", \"text\": \"几点？\", "
        "\"options\": [\"早 8 点\", \"晚 8 点\", \"其他\"]}, ...]}（可多项；问题可给 "
        "2–4 个选项供用户按数字选择，无 options 视为自由文本问答）\n"
        "  表达不了   {\"reject\": {\"reason\": \"说明缺什么能力\"}}";
  }

  std::string ir_prompt(std::string const& intent,
                        std::vector<std::pair<std::string, std::string>> const& qa,
                        Json const& plan) const {
    std::string s =
        "根据意图和已确认计划，一次性生成整段可执行 Workflow（顶层 JSON，不包围栏、不调工具）。\n"
        "顶层形状：{\"id\":\"wf_xxx\", [\"trigger\":{...} 可省略], \"nodes\":[...]}；"
        "trigger 省略 = 宿主手工触发（wfr 直接拉起），不做调度唤醒。\n"
        "给出 trigger 时：{\"source\":\"time\","
        " \"timing\":{\"kind\":\"schedule\",\"schedule\":\"5字段cron\"}}\n"
        "节点写法：\n"
        "· tool：{\"id\":\"fetch\",\"tool\":\"fetch_price\",\"parameters\":{...},"
        " \"output_schema\":{\"price\":\"number\",\"in_stock\":\"boolean\"},"
        " \"next\":\"check\",\"on_error\":{\"retry\":2,\"fallback\":\"web_backup\"}}\n"
        "· expr：{\"id\":\"check\",\"expr\":\"$_.result.price != null && "
        "$_.result.price < 800\",\"then\":\"notify\",\"else\":\"_turn_end\"}\n"
        "· label：{\"id\":\"poll_start\",\"kind\":\"label\"}；"
        "goto：{\"id\":\"goto_poll\",\"goto\":\"poll_start\",\"max\":4}\n"
        "规则：id 字母开头（不以下划线开头、不叫 result）；循环只能用 label+goto；"
        "_turn_end 只出现在 next/then/else 里（不写节点）；trigger 可省略（省略 = 手工触发），"
        "给定时不带 condition；"
        "节点 id 即 tool 输出帧名（下游用 $<id>.result.<字段> 引用，字段必须已被 output_schema 声明）。\n"
        "诚实性：无下单工具就写『提醒用户确认下单』，绝不写『已自动下单』。\n";
    // B3：把宿主真正会提供的 config 键写进提示词。不说的话模型会自己发明键名
    // （真机实测它写了 @config.item_id / @config.threshold，而宿主只提供 product_id），
    // 运行期取不到就静默变 null，直到工具读到 null 才炸。
    if (!opts.config_keys.empty()) {
      std::string keys;
      for (std::string const& k : opts.config_keys) {
        if (!keys.empty()) keys += ", ";
        keys += k;
      }
      s += "【可用配置键】运行前由宿主提供，且只能用这些（写法 @config.<键>，"
           "不得发明其它键）：" +
           keys + "\n";
    }
    s += "\n【意图】" + intent + "\n";
    if (!qa.empty()) {
      s += "【澄清问答】\n";
      for (auto const& [q, a] : qa) s += "Q: " + q + "\nA: " + a + "\n";
    }
    s += "【已确认计划】\n" + plan.dump(2) + "\n";
    s += "【可用工具目录】\n" + tool_catalog(*registry) +
         "\n输出：只输出一个 JSON 对象（{id, 可选 trigger, nodes}），不要任何解释。";
    return s;
  }

  // ---- PlanStage ----

  struct PlanOutcome {
    enum class K { Plan, Clarify, Reject, Fail };
    K k = K::Fail;
    Json plan = Json::object();
    Clarification clarify;
    std::string reason;
  };

  PlanOutcome plan_once(std::string const& intent,
                        std::vector<std::pair<std::string, std::string>> const& qa) {
    std::vector<Message> msgs;
    msgs.push_back(system_message(system_prompt()));
    std::string user = intent;
    if (!qa.empty()) {
      user += "\n\n【澄清问答】\n";
      for (auto const& [q, a] : qa) user += "Q: " + q + "\nA: " + a + "\n";
    }
    msgs.push_back(human_message(user));

    for (int attempt = 0; attempt <= opts.plan_max_retries; ++attempt) {
      std::string err;
      auto m = call(msgs, /*is_plan=*/true, err);
      if (!m) {
        PlanOutcome o;
        o.k = PlanOutcome::K::Fail;
        o.reason = err.empty() ? "plan 段：模型调用失败" : err;
        return o;
      }
      auto obj = extract_json(m->text());
      if (!obj) {
        msgs.push_back(human_message("输出不是合法 JSON。只输出一个 JSON 对象（不包围栏、不解释）。"));
        continue;
      }
      // 三态分派。纪律与 IR 的节点分类一致：**歧义即错**（同时给多个决定性键不按
      // "第一个命中"定），且每一态的形状都要真的成立 —— 否则宁可重试，
      // 不能把坏输出当成功返回（原先 questions 为空 / plan 非对象都会被当成成功）。
      Json const& o = *obj;
      int const hits = (o.contains("plan") ? 1 : 0) + (o.contains("questions") ? 1 : 0) +
                       (o.contains("reject") ? 1 : 0);
      if (hits > 1) {
        msgs.push_back(human_message(
            "三态必须**只选一个**（plan / questions / reject），你给了多个。请重出。"));
        continue;
      }
      if (o.contains("plan")) {
        if (!o["plan"].is_object()) {
          msgs.push_back(human_message("\"plan\" 必须是对象。请重出。"));
          continue;
        }
        PlanOutcome po;
        po.k = PlanOutcome::K::Plan;
        po.plan = o["plan"];
        return po;
      }
      if (o.contains("questions")) {
        std::vector<ClarifyQuestion> qs =
            o["questions"].is_array() ? questions_from_json(o["questions"])
                                      : std::vector<ClarifyQuestion>{};
        // 空集（含"每项都缺 id 被过滤光"）一律视为不合法：否则会话会落盘一份
        // 用户无法回答的空问卷，只能拿空 answers 续跑。
        if (qs.empty()) {
          msgs.push_back(human_message(
              "\"questions\" 必须是非空数组，且每项含非空 id（text 可选）。请重出。"));
          continue;
        }
        PlanOutcome po;
        po.k = PlanOutcome::K::Clarify;
        po.clarify.questions = std::move(qs);
        po.clarify.round = 1;
        return po;
      }
      if (o.contains("reject")) {
        if (!o["reject"].is_object()) {
          msgs.push_back(human_message("\"reject\" 必须是对象。请重出。"));
          continue;
        }
        PlanOutcome po;
        po.k = PlanOutcome::K::Reject;
        // reason 也来自模型：非字符串就退回默认，别让 value() 抛类型错穿出去
        if (auto it = o["reject"].find("reason"); it != o["reject"].end() && it->is_string())
          po.reason = it->get<std::string>();
        else
          po.reason = "未说明原因";
        return po;
      }
      msgs.push_back(human_message(
          "输出不合法：必须三态之一（{\"plan\":...} | {\"questions\":[...]} | {\"reject\":...}）。请重出。"));
    }
    PlanOutcome o;
    o.k = PlanOutcome::K::Fail;
    o.reason = "plan 段多次输出不合法（重试耗尽）";
    return o;
  }

  // ---- IRStage ----

  WorkflowGenerationResult ir_stage(std::string const& intent,
                                    std::vector<std::pair<std::string, std::string>> const& qa,
                                    Json const& plan) {
    std::vector<Message> msgs;
    msgs.push_back(system_message(system_prompt()));
    msgs.push_back(human_message(ir_prompt(intent, qa, plan)));

    std::string last_error;
    for (int attempt = 0; attempt <= opts.ir_max_retries; ++attempt) {
      std::string err;
      auto m = call(msgs, /*is_plan=*/false, err);
      if (!m) {
        last_error = err.empty() ? "IR 段：模型调用失败" : err;
        break;
      }
      auto obj = extract_json(m->text());
      if (!obj) {
        last_error = "IR 输出不是合法 JSON";
        msgs.push_back(human_message("输出不是合法 JSON。请整体重写为一个顶层 JSON 对象 {id, trigger, nodes}。"));
        continue;
      }
      auto wi = WorkflowIR::parse(*obj);
      if (!wi) {
        last_error = "IR 结构解析失败：" + wi.error();
        msgs.push_back(human_message("IR 结构解析失败：" + wi.error() + "。请整体重写为完整 IR JSON。"));
        continue;
      }
      // 声明了 config 契约才启用 VR4（空 = 不约束，保持旧行为）
      ValidatorOptions vopts{registry};
      if (!opts.config_keys.empty()) vopts.config_keys = opts.config_keys;
      ValidationResult vr = validate(*wi, vopts);
      if (vr.ok()) {
        WorkflowGenerationResult out;
        out.kind = WorkflowGenerationResult::Kind::Generated;
        out.generated = GeneratedWorkflow{*wi, *obj};
        return out;
      }
      last_error = "IR 校验未过：" + vr.describe();
      msgs.push_back(human_message("IR 校验未过，请整体重写并按提示改对：\n" + last_error));
    }

    // 这里是 Impl 内部（fill_fail_result 定义在其后，故直接赋值）。
    // 语义同 fill_fail_result：IR 生成失败是 Failed，不是模型拒绝。
    WorkflowGenerationResult out;
    out.kind = WorkflowGenerationResult::Kind::Failed;
    out.failed = Failure{"IR 生成失败（fail-closed）：" + last_error};
    return out;
  }

  // 会话持久化
  std::string persist_session(std::string const& intent, Clarification const& c,
                              std::vector<std::pair<std::string, std::string>> const& qa) {
    std::string const sid = "sess-" + std::to_string(++counter);
    store().put(kSessionNs, sid,
                Json{{"intent", intent},
                     {"round", c.round},
                     {"questions", questions_to_json(c.questions)},
                     {"qa", qa_to_json(qa)}});
    return sid;
  }

  void update_session(std::string const& sid, int round,
                      std::vector<ClarifyQuestion> const& questions,
                      std::vector<std::pair<std::string, std::string>> const& qa) {
    Json sess{
        {"round", round},
        {"questions", questions_to_json(questions)},
        {"qa", qa_to_json(qa)},
    };
    store().put(kSessionNs, sid, sess);
  }
};

WorkflowGenerator::WorkflowGenerator(model::ChatModel& model, tool::ToolRegistry const& registry,
                                     state::Store* store, GeneratorOptions opts)
    : impl_(std::make_shared<Impl>()) {
  impl_->model = &model;
  impl_->registry = &registry;
  impl_->external_store = store;
  impl_->opts = std::move(opts);
}

WorkflowGenerator::~WorkflowGenerator() = default;

UsageReport const& WorkflowGenerator::usage() const { return impl_->usage; }
std::string const& WorkflowGenerator::last_raw() const { return impl_->last_raw; }

namespace {

void fill_clarify_result(WorkflowGenerationResult& out, Clarification c) {
  out.kind = WorkflowGenerationResult::Kind::Clarify;
  out.clarify = std::move(c);
  out.rejected.reset();
  out.generated.reset();
}

void fill_reject_result(WorkflowGenerationResult& out, std::string reason) {
  out.kind = WorkflowGenerationResult::Kind::Rejected;
  out.rejected = Rejection{std::move(reason)};
  out.clarify.reset();
  out.generated.reset();
  out.failed.reset();
}

// 失败（模型调用失败 / 预算耗尽 / 重试耗尽）—— 刻意与 Rejected 分开，
// 否则验收矩阵分辨不出"模型拒绝"与"环境/预算出问题"。
void fill_fail_result(WorkflowGenerationResult& out, std::string reason) {
  out.kind = WorkflowGenerationResult::Kind::Failed;
  out.failed = Failure{std::move(reason)};
  out.clarify.reset();
  out.generated.reset();
  out.rejected.reset();
}

}  // namespace

WorkflowGenerationResult WorkflowGenerator::run(std::string const& intent) {
  // 预算按**一次生成**计，而不是对象生命周期：否则复用同一个 generator 跑多轮时
  // （wf_eval 的 12 例矩阵就是），前几例把预算吃光，后面全部莫名"预算耗尽"。
  impl_->calls = 0;
  auto po = impl_->plan_once(intent, {});
  switch (po.k) {
    case Impl::PlanOutcome::K::Plan: {
      auto r = impl_->ir_stage(intent, {}, po.plan);
      return r;
    }
    case Impl::PlanOutcome::K::Clarify: {
      po.clarify.session_id = impl_->persist_session(intent, po.clarify, {});
      WorkflowGenerationResult r;
      fill_clarify_result(r, std::move(po.clarify));
      return r;
    }
    case Impl::PlanOutcome::K::Reject: {
      WorkflowGenerationResult r;
      fill_reject_result(r, po.reason);
      return r;
    }
    case Impl::PlanOutcome::K::Fail: {
      WorkflowGenerationResult r;
      fill_fail_result(r, po.reason);  // 模型调用失败/重试耗尽 ≠ 模型拒绝
      return r;
    }
  }
  WorkflowGenerationResult r;  // 不可达
  fill_fail_result(r, "计划生成内部错误");
  return r;
}

WorkflowGenerationResult WorkflowGenerator::run_with_qa(
    std::string const& intent, std::vector<std::pair<std::string, std::string>> const& qa) {
  impl_->calls = 0;  // 同上：每次生成重新计预算
  auto po = impl_->plan_once(intent, qa);
  if (po.k == Impl::PlanOutcome::K::Plan) return impl_->ir_stage(intent, qa, po.plan);
  WorkflowGenerationResult r;
  if (po.k == Impl::PlanOutcome::K::Clarify) {
    fill_clarify_result(r, std::move(po.clarify));
  } else if (po.k == Impl::PlanOutcome::K::Reject) {
    fill_reject_result(r, po.reason);
  } else {
    fill_fail_result(r, po.reason);  // Fail ≠ Reject
  }
  return r;
}

std::expected<WorkflowGenerationResult, std::string> WorkflowGenerator::submit_answers(
    std::string const& session_id,
    std::vector<std::pair<std::string, std::string>> const& answers) {
  // 1) 取会话
  auto item = impl_->store().get(kSessionNs, session_id);
  if (!item) return std::unexpected("会话不存在：" + session_id);
  Json const sess = item->value;

  std::string const intent = sess.value("intent", "");
  int const round = sess.value("round", 1);
  auto const q_hist = qa_from_json(sess.value("qa", Json::array()));
  auto const asked = questions_from_json(sess.value("questions", Json::array()));

  // 2) answers 必须恰好覆盖提出的问题（id 一一对应）
  std::vector<std::string> asked_ids;
  for (ClarifyQuestion const& q : asked) asked_ids.push_back(q.id);
  std::vector<std::string> answer_ids;
  std::vector<std::pair<std::string, std::string>> qa;
  for (auto const& [id, a] : answers) {
    answer_ids.push_back(id);
    qa.emplace_back(id, a);
  }
  std::sort(asked_ids.begin(), asked_ids.end());
  std::sort(answer_ids.begin(), answer_ids.end());
  if (asked_ids != answer_ids) {
    std::vector<std::string> missing, extra;
    std::set_difference(asked_ids.begin(), asked_ids.end(), answer_ids.begin(),
                        answer_ids.end(), std::back_inserter(missing));
    std::set_difference(answer_ids.begin(), answer_ids.end(), asked_ids.begin(),
                        asked_ids.end(), std::back_inserter(extra));
    std::string msg = "answers 与提出过的问题不匹配";
    if (!missing.empty()) msg += "；缺少对以下问题的回答：" + missing[0];
    if (!extra.empty()) msg += "；包含未被提出的问题 id：" + extra[0];
    return std::unexpected(msg);
  }
  // 历史 + 本轮
  std::vector<std::pair<std::string, std::string>> qa_hist = q_hist;
  qa_hist.insert(qa_hist.end(), qa.begin(), qa.end());

  // 3) 续跑 plan
  auto po = impl_->plan_once(intent, qa_hist);
  WorkflowGenerationResult r;
  if (po.k == Impl::PlanOutcome::K::Plan) return impl_->ir_stage(intent, qa_hist, po.plan);
  if (po.k == Impl::PlanOutcome::K::Clarify) {
    int const new_round = round + 1;
    if (new_round > impl_->opts.clarify_max_rounds) {
      fill_reject_result(r, "澄清轮数超限（最多 " +
                                std::to_string(impl_->opts.clarify_max_rounds) + " 轮）");
      return r;
    }
    po.clarify.round = new_round;
    po.clarify.session_id = session_id;
    impl_->update_session(session_id, new_round, po.clarify.questions, qa_hist);
    fill_clarify_result(r, std::move(po.clarify));
    return r;
  }
  fill_reject_result(r, po.reason);
  return r;
}

}  // namespace cppchain::workflow