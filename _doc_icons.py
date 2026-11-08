import io
p = r"E:\Work\VSC\POSCommander2\план.txt"
s = io.open(p, encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in s else "\n"
anchor = "  - Перетаскивание из Проводника: файлы копируются в панель (проверено пользователем 08.11.2026)."
add = nl.join([
    anchor,
    "  - Иконки как в TC: «..» — стрелка вверх, значки конкретных папок (Desktop/Documents/Pictures",
    "    получают свои), иконки из файла для exe/lnk/ico/msi, красная метка у скрытых (08.11.2026).",
    "    Замер на 100 000 файлов после правок: окно 111 мс, память 36,8 МБ.",
])
assert s.count(anchor) == 1
s = s.replace(anchor, add, 1)

anchor2 = "## Перетаскивание"
rp = r"E:\Work\VSC\POSCommander2\README.md"
r = io.open(rp, encoding="utf-8", newline="").read()
if "## Иконки" not in r:
    note = """## Иконки

- строка `..` — стрелка вверх, как в TC;
- у папок запрашивается иконка по конкретному пути: Desktop, Documents, Pictures и другие
  узнаваемые каталоги получают свои значки;
- у `.exe`, `.lnk`, `.ico`, `.msi` иконка берётся из самого файла (у ярлыка — со стрелкой);
- скрытые и системные помечаются красной полосой на иконке.

"""
    r = r.replace("## Перетаскивание", note + "## Перетаскивание", 1)
    io.open(rp, "w", encoding="utf-8", newline="").write(r)
    print("README обновлён")
io.open(p, "w", encoding="utf-8", newline="").write(s)
print("план обновлён")
