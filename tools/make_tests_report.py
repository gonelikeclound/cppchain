#!/usr/bin/env python3
""" wfr CGI · 测试汇总：把 tmp/gtest_json/*.json（8 个 gtest 二进制的 JSON 输出）合并成
    web/tests_report.json（前端「测试」tab 读取的静态快照）。

    输入 schema（gtest --gtest_output=json 的产物，见 gtest.cc JsonUnitTestResultPrinter）：
        { tests, failures, disabled, errors, time, testsuites: [
            { name, tests, failures, testsuite: [
                { name, classname, status:"RUN", result:"COMPLETED", time, file, line,
                  failures: null | [{ failure: "文件:行号\\nmessage", type:"" }] } ] } ] }
    输出：{ generated_at, binaries: [ { name, total, passed, failed, suites: [
                { name, total, failed, tests: [
                    { name, status:"ok|fail", ms, failure } ] } ] } ] }
    纯 stdlib，无第三方依赖。
"""
import argparse
import glob
import json
from datetime import datetime, timezone


def _ms(time_val) -> int:
    """gtest JSON 的 time 是字符串 '0.006s'; 兼容数字（旧版/其它工具）。超时兜底 0。"""
    if isinstance(time_val, (int, float)):
        return round(float(time_val) * 1000) if time_val else 0
    if isinstance(time_val, str):
        try:
            s = time_val.strip().rstrip("s").strip()
            return round(float(s) * 1000)
        except (ValueError, TypeError):
            return 0
    return 0


def extract(f_path: str) -> dict:
    with open(f_path, encoding="utf-8") as f:
        j = json.load(f)
    suites = []
    for suite in j.get("testsuites", []):
        cases = []
        for case in suite.get("testsuite", []):
            fails = case.get("failures") or []
            msg = None
            if fails:
                msg = (fails[0].get("failure") or "").strip()
                if not msg:  # 兜底：有些版本只有 type 无 failure
                    msg = fails[0].get("type") or "失败（无消息）"
            cases.append({
                "name": case.get("name", ""),
                "status": "fail" if fails else "ok",
                "ms": _ms(case.get("time")),
                "failure": msg,
            })
        suites.append({
            "name": suite.get("name", ""),
            "total": suite.get("tests", len(cases)),
            "failed": len([c for c in cases if c["status"] == "fail"]),
            "tests": cases,
        })
    return {
        "name": j.get("name", ""),
        "total": j.get("tests", 0),
        "failed": j.get("failures", 0),
        "suites": suites,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="gtest JSON → web/tests_report.json 汇总")
    ap.add_argument("src_dir", help="gtest JSON 输出目录（如 tmp/gtest_json）")
    ap.add_argument("out", help="输出文件路径（如 web/tests_report.json）")
    args = ap.parse_args()

    files = sorted(glob.glob(f"{args.src_dir}/*.json"))
    if not files:
        print(f"[make_tests_report] 未找到 JSON：{args.src_dir}/*.json")
        return 2

    binaries = []
    for f in files:
        try:
            b = extract(f)
        except Exception as e:  # noqa: BLE001 —— 单个坏文件不致整体失败
            print(f"[make_tests_report] 跳过 {f}: {e}")
            continue
        if not b["suites"]:
            print(f"[make_tests_report] 空结果（可能无测试注册）：{f}")
            continue
        passed = b["total"] - b["failed"]
        # gtest 顶层 name 恒为 "AllTests"（不可区分）→ 用**文件名**作二进制名（如 workflow_tests）
        b["name"] = f.rsplit("/", 1)[-1].removesuffix(".json")
        # suite 名排序稳定输出；case 保持 gtest 原序（即声明序）
        b["suites"].sort(key=lambda s: s["name"])
        binaries.append({
            "name": b["name"],
            "total": b["total"],
            "passed": passed,
            "failed": b["failed"],
            "suites": b["suites"],
        })

    report = {
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "binaries": binaries,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
        f.write("\n")

    t = sum(b["total"] for b in binaries)
    p = sum(b["passed"] for b in binaries)
    fl = sum(b["failed"] for b in binaries)
    print(f"[make_tests_report] {args.out}: {len(binaries)} binaries · {t} tests "
          f"({p} passed / {fl} failed)")
    return 0 if fl == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())