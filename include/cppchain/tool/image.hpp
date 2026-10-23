// cppchain —— tool/image.hpp
// 取图工具（read_image）：本地文件 / URL → {media_type, data(base64), source, bytes}，
// 供多模态（ImageBlock）注入。视觉闭环的用法：
//   1) 调 read_image 拿 {media_type, data}；
//   2) 用 make_image_message(text, media_type, base64) 构建带图 Human 消息；
//   3) 交给 ChatModel（视觉端点，如 qwen-vl）。
#pragma once

#include <memory>
#include <string>

#include "cppchain/http/transport.hpp"
#include "cppchain/message/message.hpp"
#include "cppchain/tool/tool.hpp"

namespace cppchain::tool {

// read_image 工具：args 恰好一个 —— path（本地图片文件）| url（远程图片）。
// 返回 {media_type, data(base64), source, bytes}。read_only（只读）。
// URL 路径需要注入 http transport（同 web_search 纪律）；无 transport → URL 拒绝。
[[nodiscard]] ToolSpec make_read_image_tool(std::shared_ptr<http::Transport> transport);

// 把 {media_type, data(base64)} 组装成一条带图的 Human 消息（TextBlock + ImageBlock），
// 供 ChatModel 视觉调用。text 是随图的问题文案。
[[nodiscard]] message::Message make_image_message(std::string text, std::string media_type,
                                                  std::string base64_data);

}  // namespace cppchain::tool