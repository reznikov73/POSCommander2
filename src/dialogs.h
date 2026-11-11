// Диалоги: ввод строки и просмотрщик текста.
#pragma once

#include "common.h"
#include "ftp.h"

// Модальный ввод строки. Возвращает false при отмене.
// Диалог распаковки: каталог назначения и пароль (пусто — без пароля).
bool UnpackDialog(HWND owner, const std::wstring& archiveName, std::wstring& target, std::wstring& password);

bool InputBox(HWND owner, const std::wstring& title, const std::wstring& prompt, std::wstring& value,
              bool isPath);

// Параметры копирования/перемещения (F5/F6), как в TC: путь, маска, «перезаписать», «в фоне».
struct CopyMoveOptions {
    std::wstring target;
    std::wstring mask;          // пусто — все файлы
    bool overwriteAll = false;  // не спрашивать при совпадении имён
    bool background = false;    // поставить в очередь операций
};

bool CopyMoveBox(HWND owner, const std::wstring& title, const std::wstring& prompt, CopyMoveOptions& options);

// Подключение к FTP: сохранённые профили, адрес, порт, логин, пароль, пассивный режим.
struct FtpConnectOptions {
    FtpSite site;
    bool saveSite = false;
};

bool FtpConnectBox(HWND owner, FtpConnectOptions& options, const std::vector<FtpSite>& sites);

// Окно прогресса FTP-передачи. Одно на приложение; Update возвращает false при отмене.
void FtpProgressOpen(HWND owner, const std::wstring& title, const std::wstring& target = L"");
// filesDone < 0 — счётчик файлов не показывать; bytesDone — всего скопировано.
bool FtpProgressUpdate(const std::wstring& file, ULONGLONG done, ULONGLONG total,
                       int filesDone = -1, ULONGLONG bytesDone = 0);
void FtpProgressClose();

// Просмотр файла (F3) в отдельном окне.
void ShowLister(HWND owner, const std::wstring& path);

// «О программе»: версия, разработчик и контакты (ссылки кликабельны).
void ShowAbout(HWND owner);
