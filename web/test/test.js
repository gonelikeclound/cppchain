/* wfr · 测试报告页逻辑：读取静态快照 ../tests_report.json 并渲染（纯静态，不碰 CGI）。 */
"use strict";

const $ = (id) => document.getElementById(id);

function escapeHtml(s) {
  return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

// 毫秒 → "1.2s" / "340ms" / "1m 2s" 可读
function fmtMs(ms) {
  if (ms >= 60000) return (ms / 60000).toFixed(1) + "m";
  if (ms >= 1000) return (ms / 1000).toFixed(1) + "s";
  return Math.round(ms) + "ms";
}

function renderStats(report, totalMs) {
  const failed = report.binaries.reduce((a, b) => a + b.failed, 0);
  const passed = report.binaries.reduce((a, b) => a + b.passed, 0);
  const total = report.binaries.reduce((a, b) => a + b.total, 0);
  const cards = { total, ok: passed, bad: failed, time: fmtMs(totalMs) };
  for (const [k, v] of Object.entries(cards)) {
    const card = $("stats").querySelector(".stat-card." + k);
    if (card) card.querySelector("b").textContent = String(v);
  }
  // 整体基调：有失败 → 页头统计失败卡红；全绿 → 通过卡绿高亮
  document.querySelectorAll(".stat-card").forEach(c => c.classList.remove("warn"));
  if (failed > 0) $("stats").querySelector(".stat-card.bad").classList.add("warn");
}

function loadTestsReport() {
  const box = $("tests-table");
  fetch("../tests_report.json", { cache: "no-store" })
    .then(r => {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(report => {
      if (!report || !Array.isArray(report.binaries) || report.binaries.length === 0) {
        throw new Error("空报告（先 ./run-tests-report.sh）");
      }
      let totalMs = 0, html = "";
      // 二进制按 通过率升序排（失败的排最前），套件保持报告顺序
      const bins = [...report.binaries].sort((a, b) => (b.passed / b.total) - (a.passed / a.total));
      for (const bin of bins) {
        totalMs += bin.suites.reduce((s, su) => s + su.tests.reduce((x, t) => x + (t.ms || 0), 0), 0);
        const bad = bin.failed > 0;
        html +=
          '<details class="tests-binary' + (bad ? " bad" : "") + '"' + (bad ? " open" : "") + ">" +
          "<summary>" +
          "<span class='codename'>" + escapeHtml(bin.name) + "</span>" +
          "<span class='bcounts'>" +
          "<span class='b-total'>" + bin.total + " 例</span>" +
          "<span class='b-ok'>" + bin.passed + " 过</span>" +
          (bad ? "<span class='b-bad'>" + bin.failed + " 败</span>" : "") +
          "</span>" +
          "</summary>" +
          '<table class="tests-table"><thead><tr>' +
          "<th>套件</th><th>用例</th><th>状态</th><th>耗时</th><th>失败原因</th>" +
          "</tr></thead><tbody>";
        for (const s of bin.suites) {
          for (const t of s.tests) {
            const ok = t.status === "ok";
            html +=
              "<tr" + (ok ? "" : " class='fail-row'") + ">" +
              "<td>" + escapeHtml(s.name) + "</td>" +
              "<td>" + escapeHtml(t.name) + "</td>" +
              "<td class='" + (ok ? "ok" : "bad") + "'>" + (ok ? "✓" : "✗") + "</td>" +
              "<td class='ms'>" + fmtMs(t.ms) + "</td>" +
              "<td class='fail-detail'>" + (ok ? "" : escapeHtml(t.failure || "失败")) + "</td>" +
              "</tr>";
          }
        }
        html += "</tbody></table></details>";
      }
      renderStats(report, totalMs);
      box.innerHTML = html;
    })
    .catch(e => {
      $("stats").querySelector(".stat-card.total").querySelector("b").textContent = "!";
      box.innerHTML = '<div class="error-line">读取测试报告失败: ' + escapeHtml(e.message) +
        '（先跑 ./run-tests-report.sh 生成 web/tests_report.json）</div>';
    });
}

// boot：页面加载即渲染
loadTestsReport();
