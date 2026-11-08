"""Export one AutoClaw 2.x chat session from runtime.sqlite into human-readable files.

Usage:
    python export_session.py [session-title-fragment] [output-dir]

Defaults: title fragment "POSCommander Альтернатив", output dir = current working dir.
Read-only: the source database is opened in mode=ro and is never modified.
"""

from __future__ import annotations

import glob
import hashlib
import json
import os
import shutil
import sqlite3
import sys
from datetime import datetime
from typing import Any

TITLE_FRAGMENT = sys.argv[1] if len(sys.argv) > 1 else "POSCommander Альтернатив"
OUT_DIR = sys.argv[2] if len(sys.argv) > 2 else os.getcwd()

ACCOUNTS_ROOT = os.path.join(
    os.path.expanduser("~"), "AppData", "Roaming", "AutoClaw-oversea-official", "accounts"
)
RESULT_PREVIEW_CHARS = 2000


def find_session() -> tuple[str, sqlite3.Row, sqlite3.Connection]:
    candidates = glob.glob(os.path.join(ACCOUNTS_ROOT, "*", "runtime", "runtime.sqlite"))
    for db in candidates:
        try:
            con = sqlite3.connect("file:" + db + "?mode=ro", uri=True)
            con.row_factory = sqlite3.Row
            for row in con.execute(
                "select * from work_sessions where title like ? order by updated_at desc",
                ("%" + TITLE_FRAGMENT + "%",),
            ):
                count = con.execute(
                    "select count(*) from neutral_session_entries where session_id=?",
                    (row["session_id"],),
                ).fetchone()[0]
                if count:
                    return db, row, con
            con.close()
        except sqlite3.Error:
            continue
    raise SystemExit("Session %r not found in any AutoClaw 2.x account database" % TITLE_FRAGMENT)


def local_time(value: str | None) -> str:
    if not value:
        return "—"
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone().strftime("%d.%m.%Y %H:%M")
    except ValueError:
        return value


def human_size(num: int) -> str:
    for unit in ("Б", "КБ", "МБ"):
        if num < 1024 or unit == "МБ":
            return ("%d %s" % (num, unit)) if unit == "Б" else ("%.1f %s" % (num, unit))
        num /= 1024.0
    return "%d Б" % num


def fmt_input(tool: str, data: dict[str, Any]) -> str:
    """Compact one-line description of a tool call."""
    if tool == "exec":
        cmd = (data.get("command") or "").replace("\r", " ").replace("\n", " ⏎ ")
        return "`%s`" % (cmd[:400] + ("…" if len(cmd) > 400 else ""))
    parts = []
    for key, value in data.items():
        text = json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else value
        text = text.replace("\r", " ").replace("\n", " ⏎ ")
        if len(text) > 220:
            text = text[:220] + "…"
        parts.append("%s=%s" % (key, text))
    return ", ".join(parts) if parts else "—"


def main() -> None:
    db, session, con = find_session()
    sid = session["session_id"]

    workspace = None
    if session["workspace_id"]:
        row = con.execute(
            "select display_name, root_path from work_workspaces where workspace_id=?",
            (session["workspace_id"],),
        ).fetchone()
        workspace = dict(row) if row else None

    entries = con.execute(
        "select session_seq, entry_id, entry_type, run_id, committed_at, payload "
        "from neutral_session_entries where session_id=? order by session_seq",
        (sid,),
    ).fetchall()
    parsed = [(e, json.loads(e["payload"])["data"]) for e in entries]

    # Final answer per run, and the request text per run (work_runs.text is empty for the first run).
    answer_text: dict[str, str] = {}
    request_text: dict[str, str] = {}
    for e, data in parsed:
        if e["entry_type"] == "answer":
            answer_text[e["run_id"]] = (data.get("text") or "").strip()
        elif e["entry_type"] == "request" and e["run_id"] not in request_text:
            request_text[e["run_id"]] = (data.get("text") or data.get("effectivePrompt") or "").strip()

    runs = con.execute(
        "select run_id, status, text, connection_provider, connection_model, created_at, usage_json "
        "from work_runs where session_id=? order by accept_order",
        (sid,),
    ).fetchall()
    run_by_id = {r["run_id"]: r for r in runs}

    artifacts = con.execute(
        "select display_name, media_type, byte_length, source_path, reference "
        "from governed_tool_artifacts where session_id=? order by created_at",
        (sid,),
    ).fetchall()

    os.makedirs(OUT_DIR, exist_ok=True)
    slug = "POSCommander-Альтернатив_" + (session["created_at"] or "")[:10]

    # ---------- raw JSONL ----------
    jsonl_path = os.path.join(OUT_DIR, slug + ".jsonl")
    with open(jsonl_path, "w", encoding="utf-8", newline="\n") as fh:
        for e in entries:
            fh.write(
                json.dumps(
                    {
                        "sessionId": sid,
                        "sessionSeq": e["session_seq"],
                        "entryId": e["entry_id"],
                        "entryType": e["entry_type"],
                        "runId": e["run_id"],
                        "committedAt": e["committed_at"],
                        "payload": json.loads(e["payload"]),
                    },
                    ensure_ascii=False,
                )
                + "\n"
            )

    # ---------- attachments / artifacts ----------
    assets_dir = os.path.join(OUT_DIR, "assets")
    copied: list[str] = []
    for e in entries:
        if e["entry_type"] != "attachment":
            continue
        att = json.loads(e["payload"])["data"]["attachment"]
        base = workspace["root_path"] if workspace else os.path.dirname(OUT_DIR)
        src = os.path.join(base, att["workspacePath"].replace("/", os.sep))
        if os.path.exists(src):
            os.makedirs(assets_dir, exist_ok=True)
            dst = os.path.join(assets_dir, att["name"])
            if not os.path.exists(dst):
                shutil.copy2(src, dst)
            copied.append("assets/%s — %s, %s" % (att["name"], att["mediaType"], human_size(os.path.getsize(dst))))

    blobs_dir = os.path.join(os.path.dirname(db), "artifacts", "blobs")
    artifact_notes: list[str] = []
    for a in artifacts:
        note = "- `%s` (%s, %s)" % (a["source_path"] or a["display_name"], a["media_type"], human_size(a["byte_length"]))
        digest = (a["reference"] or "").replace("artifact://", "")
        if digest:
            hits = glob.glob(os.path.join(blobs_dir, digest + "*")) + glob.glob(
                os.path.join(blobs_dir, "*", digest + "*")
            )
            if hits and not a["source_path"]:
                os.makedirs(os.path.join(assets_dir, "artifacts"), exist_ok=True)
                dst = os.path.join(assets_dir, "artifacts", a["display_name"].replace(" ", "-") + ".txt")
                shutil.copy2(hits[0], dst)
                note += " → `assets/artifacts/%s`" % os.path.basename(dst)
        artifact_notes.append(note)

    # ---------- Markdown transcript ----------
    md: list[str] = []
    md.append("# %s — история диалога" % session["title"])
    md.append("")
    md.append("Выгружено из AutoClaw 2.2.2 · экспорт %s" % datetime.now().strftime("%d.%m.%Y %H:%M"))
    md.append("")
    md.append("| | |")
    md.append("|---|---|")
    md.append("| Сессия | `%s` |" % sid)
    md.append("| Начата | %s |" % local_time(session["created_at"]))
    md.append("| Последнее сообщение | %s |" % local_time(session["updated_at"]))
    if workspace:
        md.append("| Проект | %s — `%s` |" % (workspace["display_name"], workspace["root_path"]))
    md.append("| Записей в базе | %d |" % len(entries))
    md.append("| Запросов / запусков | %d / %d |" % (
        sum(1 for e in entries if e["entry_type"] == "request"), len(runs)))
    md.append("| Источник | `%s` |" % db)
    md.append("| Полная копия | `%s` |" % os.path.basename(jsonl_path))
    md.append("")

    if runs:
        md.append("## Запуски")
        md.append("")
        md.append("| # | Время | Статус | Модель | Токены (вход/выход) | Запрос |")
        md.append("|---|---|---|---|---|---|")
        for i, r in enumerate(runs, 1):
            usage = {}
            if r["usage_json"]:
                try:
                    usage = json.loads(r["usage_json"])
                except json.JSONDecodeError:
                    usage = {}
            tokens = "%s / %s" % (usage.get("input", "—"), usage.get("output", "—")) if usage else "—"
            model = r["connection_model"] or "—"
            text = (r["text"] or "").replace("\n", " ").strip() or request_text.get(r["run_id"], "")
            text = text.replace("\n", " ").strip()
            if not text:
                text = "_продолжение предыдущего запроса_"
            md.append(
                "| %d | %s | `%s` | `%s` | %s | %s |"
                % (i, local_time(r["created_at"]), r["status"], model, tokens, text[:80] + ("…" if len(text) > 80 else ""))
            )
        md.append("")

    md.append("## Диалог")
    md.append("")

    user_no = 0
    for e, data in parsed:
        etype = e["entry_type"]
        stamp = local_time(e["committed_at"])
        final = answer_text.get(e["run_id"], "")

        if etype == "request":
            user_no += 1
            text = data.get("text") or data.get("effectivePrompt") or ""
            md.append("---")
            md.append("")
            md.append("### %d. Запрос · %s" % (user_no, stamp))
            md.append("")
            for line in text.splitlines():
                md.append("> " + line if line.strip() else ">")
            md.append("")
        elif etype == "attachment":
            att = data.get("attachment", {})
            md.append("_Вложение:_ `%s` (%s, %s)" % (att.get("name"), att.get("mediaType"), human_size(att.get("byteLength", 0))))
            md.append("")
        elif etype == "assistant":
            texts = [c.get("text", "") for c in data.get("content", []) if c.get("type") == "text"]
            body = "\n\n".join(t for t in texts if t.strip())
            # The closing assistant entry repeats the final answer verbatim; render it once, from `answer`.
            if body.strip() and body.strip() == final:
                body = ""
            if body:
                md.append("**Ассистент** · %s" % stamp)
                md.append("")
                md.append(body)
                md.append("")
        elif etype == "answer":
            body = data.get("text") or ""
            if body:
                md.append("**Ассистент (финальный ответ)** · %s" % stamp)
                md.append("")
                md.append(body)
                md.append("")
        elif etype == "reasoning":
            thought = (data.get("text") or "").strip()
            if not thought:
                continue
            total = len(thought)
            md.append("<details><summary>рассуждение модели · %s</summary>" % stamp)
            md.append("")
            for line in thought[:1500].splitlines():
                md.append("> " + line if line.strip() else ">")
            if total > 1500:
                md.append(">")
                md.append("> … [обрезано: 1500 из %s символов, полный текст в .jsonl]" % human_size(total))
            md.append("")
            md.append("</details>")
            md.append("")
        elif etype == "tool_call":
            md.append("<details><summary><code>%s</code> · %s</summary>" % (data.get("toolName"), stamp))
            md.append("")
            md.append("```text")
            md.append(fmt_input(data.get("toolName", ""), data.get("input", {})))
            md.append("```")
            md.append("")
            md.append("</details>")
            md.append("")
        elif etype == "tool_result":
            content = data.get("content")
            if not isinstance(content, str):
                content = json.dumps(content, ensure_ascii=False, indent=2)
            total = len(content)
            trimmed = content[:RESULT_PREVIEW_CHARS]
            status = data.get("status") or "—"
            md.append("<details><summary>результат <code>%s</code> · %s</summary>" % (data.get("toolName"), status))
            md.append("")
            md.append("```text")
            md.append(trimmed.replace("```", "ˋˋˋ"))
            if total > RESULT_PREVIEW_CHARS:
                md.append("… [обрезано: %s из %s символов, полный текст в .jsonl]" % (human_size(RESULT_PREVIEW_CHARS), human_size(total)))
            md.append("```")
            if data.get("error"):
                md.append("")
                md.append("**Ошибка:** %s" % str(data["error"])[:600])
            md.append("")
            md.append("</details>")
            md.append("")

    md_path = os.path.join(OUT_DIR, slug + ".md")
    with open(md_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(md).rstrip() + "\n")

    sha = hashlib.sha256(open(jsonl_path, "rb").read()).hexdigest()
    sizes = {os.path.basename(p): os.path.getsize(p) for p in (md_path, jsonl_path)}

    readme = [
        "# История сессии «%s»" % session["title"],
        "",
        "Копия переписки из AutoClaw 2.2.2, привязанной к проекту `%s`." % (workspace["root_path"] if workspace else "—"),
        "",
        "## Файлы",
        "",
        "| Файл | Размер | Что внутри |",
        "|---|---|---|",
        "| `%s` | %s | читаемый транскрипт: запросы, ответы, вызовы инструментов, обрезанные результаты |"
        % (os.path.basename(md_path), human_size(sizes[os.path.basename(md_path)])),
        "| `%s` | %s | полная копия всех записей без обрезки (JSON Lines, по одной записи на строку) |"
        % (os.path.basename(jsonl_path), human_size(sizes[os.path.basename(jsonl_path)])),
        "| `export_session.py` | — | скрипт, которым сделана выгрузка (только чтение базы) |",
        "",
    ]
    if copied:
        readme += ["## Вложения", ""] + ["- " + c for c in copied] + [""]
    if artifact_notes:
        readme += ["## Артефакты сессии", ""] + artifact_notes + [""]
    readme += [
        "## Источник",
        "",
        "- база: `%s`" % db,
        "- `session_id`: `%s`" % sid,
        "- `run_id`: %s" % ", ".join("`%s`" % r["run_id"] for r in runs),
        "- sha256 файла `.jsonl`: `%s`" % sha,
        "",
        "## Как повторить выгрузку",
        "",
        "```powershell",
        'python export_session.py "POSCommander Альтернатив" "E:\\Work\\VSC\\POSCommander2\\docs\\session-history"',
        "```",
        "",
        "Скрипт открывает `runtime.sqlite` в режиме `mode=ro` и ничего в нём не меняет; можно запускать при работающем AutoClaw.",
        "",
    ]
    with open(os.path.join(OUT_DIR, "README.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(readme))

    shutil.copy2(os.path.abspath(__file__), os.path.join(OUT_DIR, "export_session.py"))

    print("db        :", db)
    print("session   :", sid, "|", session["title"])
    print("entries   :", len(entries), "| runs:", len(runs))
    print("written   :")
    for name, size in sizes.items():
        print("   %-55s %s" % (name, human_size(size)))
    for c in copied:
        print("   " + c)
    print("sha256    :", sha)


if __name__ == "__main__":
    main()
