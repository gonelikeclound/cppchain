/* wfr CGI · 测试报告页逻辑：读取静态快照 ../tests_report.json 并渲染（纯静态，不碰 CGI）。 */
"use strict";

const $ = (id) => document.getElementById(id);

function escapeHtml(s) {
  return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;")
    .replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function loadTestsReport() {
  const box = $("tests-table");
  const sum = document.querySelector(".tests-summary");
  const genAt = document.getElementById("gen-at");
  fetch("../tests_report.json", { cache: "no-store" })
    .then(r => {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(report => {
      if (!report || !Array.isArray(report.binaries) || report.binaries.length === 0) {
        throw new Error("空报告（先 ./run-tests-report.sh）");
      }
      const total = report.binaries.reduce((a, b) => a + b.total, 0);
      const passed = report.binaries.reduce((a, b) => a + b.passed, 0);
      const failed = report.binaries.reduce((a, b) => a + b.failed, 0);
      sum.innerHTML =
        '<div class="tests-summary-line">' +
        '<b class="' + (failed ? "fail" : "pass") + '">' + total + " 个测试 · " +
        passed + " 通过" + (failed ? " · " + failed + " 失败" : " · 全绿") + "</b>" +
        ' <span class="dim">快照 ' + (report.generated_at || "?") + "</span></div>";
      if (genAt) genAt.textContent = report.generated_at || "";

      let html = "";
      for (const bin of report.binaries) {
        const failedSuites = bin.suites.filter(s => s.failed > 0);
        html +=
          '<details class="tests-binary"' + (failedSuites.length ? " open" : "") + ">" +
          "<summary>" +
          "<span class='codename'>" + escapeHtml(bin.name) + "</span> " +
          bin.total + " · <span class='pass'>" + bin.passed + "</span> 过" +
          (bin.failed ? " · <span class='fail'>" + bin.failed + "</span> 败" : "") +
          "</summary>" +
          '<table class="tests-table"><thead><tr>' +
          "<th>套件</th><th>用例</th><th>状态</th><th>耗时</th><th>失败原因</th>" +
          "</tr></thead><tbody>";
        for (const s of bin.suites) {
          for (const t of s.tests) {
            const ok = t.status === "ok";
            html +=
              "<tr><td>" + escapeHtml(s.name) +
              "</td><td>" + escapeHtml(t.name) +
              "</td><td class='" + (ok ? "pass" : "fail") + "'>" + (ok ? "✓" : "✗") +
              "</td><td>" + t.ms + "ms</td><td class='fail-detail'>" +
              (ok ? "" : escapeHtml(t.failure || "失败")) + "</td></tr>";
          }
        }
        html += "</tbody></table></details>";
      }
      box.innerHTML = html;
    })
    .catch(e => {
      box.innerHTML = '<div class="error-line">读取测试报告失败: ' + escapeHtml(e.message) +
        '（先跑 ./run-tests-report.sh 生成 web/tests_report.json）</div>';
    });
}

// boot：加载即渲染（页面独立，无 tab 切换）
loadTestsReport();
