// Самопроверка: прогоняет файловые операции во временном каталоге вне проекта.
#pragma once

#include <string>

// Возвращает число проваленных проверок (0 — всё в порядке).
int RunSelfTest();

// Сквозная проверка FTP против указанного сервера: host:port:user:pass.
// Возвращает число провалов; отчёт — %TEMP%\posc2-ftp-test.txt.
int RunFtpTest(const std::wstring& spec);
