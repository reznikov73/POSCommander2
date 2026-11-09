#!/usr/bin/env python3
"""Минимальный локальный FTP-сервер для проверки клиента POS Commander.

Только для разработки: слушает 127.0.0.1, без шифрования, один корень.
Поддерживает то, что реально нужно WinINet и нашему --ftp-test:

    USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE PASV EPSV PORT
    LIST NLST RETR STOR APPE DELE MKD RMD RNFR RNTO SIZE MDTM
    REST NOOP QUIT ABOR STAT

Запуск:
    python tools/ftp_test_server.py --port 2121 --user test --password test
    python tools/ftp_test_server.py --port 0          # порт выберет система

Печатает строку "LISTENING <port>" и работает до Ctrl+C.
"""

from __future__ import annotations

import argparse
import os
import socket
import stat as statmod
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field

MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]


def log(text: str) -> None:
    sys.stderr.write(time.strftime("%H:%M:%S ") + text + "\n")
    sys.stderr.flush()


@dataclass
class Session:
    conn: socket.socket
    addr: tuple
    root: str
    user: str
    password: str
    anonymous: bool
    readonly: bool
    cwd: str = "/"
    user_given: str = ""
    logged_in: bool = False
    rest_offset: int = 0
    rename_from: str = ""
    pasv: socket.socket | None = None
    transfer_type: str = "I"
    lock: threading.Lock = field(default_factory=threading.Lock)


class FtpTestServer:
    def __init__(self, root: str, user: str, password: str,
                 anonymous: bool = False, readonly: bool = False, port: int = 0, mlsd: bool = False):
        self.root = os.path.realpath(root)
        self.user = user
        self.password = password
        self.anonymous = anonymous
        self.readonly = readonly
        self.mlsd = mlsd
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.stopping = False
        self.threads: list[threading.Thread] = []

    # ---------- пути ----------

    def real_path(self, virtual: str, cwd: str = "/") -> str:
        virtual = virtual.replace("\\", "/")
        if not virtual.startswith("/"):
            virtual = cwd.rstrip("/") + "/" + virtual
        parts: list[str] = []
        for part in virtual.split("/"):
            if part in ("", "."):
                continue
            if part == "..":
                if parts:
                    parts.pop()
                continue
            parts.append(part)
        full = os.path.realpath(os.path.join(self.root, *parts))
        if full != self.root and not full.startswith(self.root + os.sep):
            raise PermissionError("path escapes root")
        return full

    @staticmethod
    def virtual_of(real: str, root: str) -> str:
        rel = os.path.relpath(real, root)
        if rel == ".":
            return "/"
        return "/" + rel.replace(os.sep, "/")

    # ---------- запуск ----------

    def serve_forever(self) -> None:
        log(f"root={self.root} user={self.user} readonly={self.readonly}")
        print(f"LISTENING {self.port}", flush=True)
        while not self.stopping:
            try:
                conn, addr = self.sock.accept()
            except OSError:
                break
            t = threading.Thread(target=self.handle_client, args=(conn, addr), daemon=True)
            t.start()
            self.threads.append(t)

    def stop(self) -> None:
        self.stopping = True
        try:
            self.sock.close()
        except OSError:
            pass

    # ---------- сессия ----------

    def handle_client(self, conn: socket.socket, addr: tuple) -> None:
        sess = Session(conn=conn, addr=addr, root=self.root, user=self.user,
                       password=self.password, anonymous=self.anonymous, readonly=self.readonly)
        f = conn.makefile("rwb", buffering=0)
        self.reply(f, "220 POS Commander test FTP ready")
        try:
            while True:
                line = f.readline()
                if not line:
                    break
                text = line.decode("utf-8", "replace").rstrip("\r\n")
                if not text:
                    continue
                cmd, _, arg = text.partition(" ")
                cmd = cmd.upper()
                log(f"{addr[0]}:{addr[1]} {cmd}" + ("" if cmd == "PASS" else f" {arg}"))
                if not self.dispatch(sess, f, cmd, arg):
                    break
        except (ConnectionError, OSError) as exc:
            log(f"session error: {exc}")
        finally:
            self.close_pasv(sess)
            try:
                f.close()
            except OSError:
                pass
            try:
                conn.close()
            except OSError:
                pass

    @staticmethod
    def reply(f, text: str) -> None:
        f.write((text + "\r\n").encode("utf-8"))

    def close_pasv(self, sess: Session) -> None:
        if sess.pasv is not None:
            try:
                sess.pasv.close()
            except OSError:
                pass
            sess.pasv = None

    def open_pasv(self, sess: Session) -> socket.socket:
        self.close_pasv(sess)
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", 0))
        s.listen(1)
        s.settimeout(10)
        sess.pasv = s
        return s

    def data_socket(self, sess: Session, f, arg: str) -> socket.socket | None:
        """Открывает канал данных (PASV уже открыт, либо PORT)."""
        if sess.pasv is not None:
            try:
                data, _ = sess.pasv.accept()
                return data
            except OSError as exc:
                log(f"pasv accept failed: {exc}")
                return None
            finally:
                self.close_pasv(sess)
        if getattr(sess, "active_addr", None):
            host, port = sess.active_addr
            try:
                return socket.create_connection((host, port), timeout=10)
            except OSError as exc:
                log(f"active connect failed: {exc}")
                return None
        self.reply(f, "425 Use PASV or PORT first")
        return None

    def dispatch(self, sess: Session, f, cmd: str, arg: str) -> bool:
        if cmd == "USER":
            sess.user_given = arg.strip()
            if self.anonymous and sess.user_given.lower() in ("anonymous", "ftp"):
                sess.logged_in = True
                self.reply(f, "230 Anonymous login ok")
            else:
                self.reply(f, "331 Password required")
            return True
        if cmd == "PASS":
            if sess.user_given == self.user and arg == self.password:
                sess.logged_in = True
                self.reply(f, "230 Login successful")
            elif self.anonymous:
                sess.logged_in = True
                self.reply(f, "230 Anonymous login ok")
            else:
                self.reply(f, "530 Login incorrect")
            return True
        if cmd == "SYST":
            self.reply(f, "215 UNIX Type: L8")
            return True
        if cmd == "FEAT":
            self.reply(f, "211-Features:")
            self.reply(f, " UTF8")
            self.reply(f, " SIZE")
            self.reply(f, " MDTM")
            self.reply(f, " PASV")
            self.reply(f, " EPSV")
            self.reply(f, " REST STREAM")
            if self.mlsd:
                self.reply(f, " MLSD")
            self.reply(f, "211 End")
            return True
        if cmd == "OPTS":
            self.reply(f, "200 OPTS ok")
            return True
        if cmd == "NOOP":
            self.reply(f, "200 NOOP ok")
            return True
        if cmd == "QUIT":
            self.reply(f, "221 Bye")
            return False
        if cmd == "ABOR":
            self.close_pasv(sess)
            self.reply(f, "226 Abort ok")
            return True
        if cmd == "STAT":
            self.reply(f, "211 POS Commander test FTP")
            return True

        if not sess.logged_in:
            self.reply(f, "530 Please login first")
            return True

        if cmd == "PWD" or cmd == "XPWD":
            self.reply(f, '257 "%s" is current directory' % sess.cwd)
            return True
        if cmd == "CWD" or cmd == "XCWD":
            try:
                real = self.real_path(arg.strip() or "/", sess.cwd)
            except PermissionError:
                self.reply(f, "550 Invalid path")
                return True
            if os.path.isdir(real):
                sess.cwd = self.virtual_of(real, self.root)
                self.reply(f, "250 Directory changed")
            else:
                self.reply(f, "550 No such directory")
            return True
        if cmd == "CDUP" or cmd == "XCUP":
            parent = os.path.dirname(sess.cwd.rstrip("/")) or "/"
            sess.cwd = parent if parent.startswith("/") else "/"
            self.reply(f, "250 Directory changed")
            return True
        if cmd == "TYPE":
            sess.transfer_type = (arg.strip() or "I").upper()[:1]
            self.reply(f, "200 Type set to " + sess.transfer_type)
            return True
        if cmd == "MODE":
            self.reply(f, "200 Mode S")
            return True
        if cmd == "STRU":
            self.reply(f, "200 Structure F")
            return True
        if cmd == "PASV":
            s = self.open_pasv(sess)
            ip, port = s.getsockname()
            p1, p2 = port // 256, port % 256
            self.reply(f, "227 Entering Passive Mode (%s,%d,%d)" %
                       (ip.replace(".", ","), p1, p2))
            return True
        if cmd == "EPSV":
            s = self.open_pasv(sess)
            port = s.getsockname()[1]
            self.reply(f, "229 Entering Extended Passive Mode (|||{port}|)")
            return True
        if cmd == "PORT":
            nums = arg.strip().split(",")
            if len(nums) != 6:
                self.reply(f, "501 Bad PORT")
                return True
            host = ".".join(nums[:4])
            port = int(nums[4]) * 256 + int(nums[5])
            sess.active_addr = (host, port)
            self.reply(f, "200 PORT ok")
            return True
        if cmd == "SIZE":
            try:
                real = self.real_path(self.join(sess.cwd, arg))
            except PermissionError:
                self.reply(f, "550 Invalid path")
                return True
            if os.path.isfile(real):
                self.reply(f, "213 %d" % os.path.getsize(real))
            else:
                self.reply(f, "550 Not a file")
            return True
        if cmd == "MDTM":
            try:
                real = self.real_path(self.join(sess.cwd, arg))
            except PermissionError:
                self.reply(f, "550 Invalid path")
                return True
            if os.path.exists(real):
                stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(real)))
                self.reply(f, "213 " + stamp)
            else:
                self.reply(f, "550 Not found")
            return True
        if cmd in ("LIST", "NLST"):
            return self.cmd_list(sess, f, cmd, arg)
        if cmd == "MLSD":
            return self.cmd_mlsd(sess, f, arg)
        if cmd == "RETR":
            return self.cmd_retr(sess, f, arg)
        if cmd in ("STOR", "APPE"):
            return self.cmd_stor(sess, f, arg, append=(cmd == "APPE"))
        if cmd == "DELE":
            return self.cmd_dele(sess, f, arg)
        if cmd == "MKD" or cmd == "XMKD":
            return self.cmd_mkd(sess, f, arg)
        if cmd == "RMD" or cmd == "XRMD":
            return self.cmd_rmd(sess, f, arg)
        if cmd == "RNFR":
            try:
                real = self.real_path(self.join(sess.cwd, arg))
            except PermissionError:
                self.reply(f, "550 Invalid path")
                return True
            if os.path.exists(real):
                sess.rename_from = real
                self.reply(f, "350 Ready for RNTO")
            else:
                self.reply(f, "550 Not found")
            return True
        if cmd == "RNTO":
            if not sess.rename_from:
                self.reply(f, "503 RNFR first")
                return True
            if self.readonly:
                self.reply(f, "550 Read-only server")
                return True
            try:
                real = self.real_path(self.join(sess.cwd, arg))
            except PermissionError:
                self.reply(f, "550 Invalid path")
                return True
            try:
                os.replace(sess.rename_from, real)
                self.reply(f, "250 Renamed")
            except OSError as exc:
                self.reply(f, "550 Rename failed: %s" % exc)
            sess.rename_from = ""
            return True
        if cmd == "REST":
            try:
                sess.rest_offset = int(arg.strip() or "0")
                self.reply(f, "350 Restarting at %d" % sess.rest_offset)
            except ValueError:
                self.reply(f, "501 Bad offset")
            return True

        self.reply(f, f"502 Command not implemented: {cmd}")
        return True

    @staticmethod
    def join(cwd: str, name: str) -> str:
        name = (name or "").strip()
        if name.startswith("/"):
            return name
        return cwd.rstrip("/") + "/" + name

    # ---------- команды с данными ----------

    def cmd_list(self, sess: Session, f, cmd: str, arg: str) -> bool:
        target_arg = arg.strip()
        if target_arg.startswith("-"):
            # LIST -al / NLST -a: опции игнорируем
            parts = target_arg.split(None, 1)
            target_arg = parts[1] if len(parts) > 1 else ""
        try:
            real = self.real_path(self.join(sess.cwd, target_arg) if target_arg else sess.cwd)
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        if not os.path.exists(real):
            self.reply(f, "550 Not found")
            return True
        data = self.data_socket(sess, f, target_arg)
        if data is None:
            return True
        self.reply(f, "150 Opening data connection")
        try:
            with data:
                if cmd == "NLST":
                    names = self.entry_names(real)
                    payload = "".join(n + "\r\n" for n in names).encode("utf-8")
                    data.sendall(payload)
                else:
                    lines = []
                    for name, full in self.entry_items(real):
                        lines.append(self.list_line(name, full))
                    data.sendall("".join(lines).encode("utf-8"))
            self.reply(f, "226 Transfer complete")
        except OSError as exc:
            log(f"list failed: {exc}")
            self.reply(f, "426 Transfer aborted")
        return True

    @staticmethod
    def entry_names(real: str) -> list[str]:
        if os.path.isfile(real):
            return [os.path.basename(real)]
        return sorted(os.listdir(real))

    def entry_items(self, real: str):
        if os.path.isfile(real):
            yield os.path.basename(real), real
            return
        for name in sorted(os.listdir(real)):
            yield name, os.path.join(real, name)

    @staticmethod
    def list_line(name: str, full: str) -> str:
        st = os.stat(full)
        is_dir = statmod.S_ISDIR(st.st_mode)
        perms = "drwxr-xr-x" if is_dir else "-rw-r--r--"
        size = 0 if is_dir else st.st_size
        mtime = time.localtime(st.st_mtime)
        now = time.time()
        if now - st.st_mtime < 180 * 24 * 3600:
            when = "%s %2d %02d:%02d" % (MONTHS[mtime.tm_mon - 1], mtime.tm_mday,
                                          mtime.tm_hour, mtime.tm_min)
        else:
            when = "%s %2d  %04d" % (MONTHS[mtime.tm_mon - 1], mtime.tm_mday, mtime.tm_year)
        return "%s 1 owner group %12d %s %s\r\n" % (perms, size, when, name)


    def cmd_mlsd(self, sess: Session, f, arg: str) -> bool:
        target_arg = arg.strip()
        try:
            real = self.real_path(self.join(sess.cwd, target_arg) if target_arg else sess.cwd)
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        if not os.path.isdir(real):
            self.reply(f, "550 Not a directory")
            return True
        data = self.data_socket(sess, f, target_arg)
        if data is None:
            return True
        self.reply(f, "150 Opening data connection")
        try:
            with data:
                lines = []
                stamp = time.strftime("%Y%m%d%H%M%S", time.gmtime(os.path.getmtime(real)))
                lines.append("type=cdir;modify=%s; .\r\n" % stamp)
                lines.append("type=pdir;modify=%s; ..\r\n" % stamp)
                for name, full in self.entry_items(real):
                    st = os.stat(full)
                    kind = "dir" if statmod.S_ISDIR(st.st_mode) else "file"
                    mtime = time.strftime("%Y%m%d%H%M%S", time.gmtime(st.st_mtime))
                    size = 0 if kind == "dir" else st.st_size
                    lines.append("type=%s;size=%d;modify=%s; %s\r\n" % (kind, size, mtime, name))
                data.sendall("".join(lines).encode("utf-8"))
            self.reply(f, "226 Transfer complete")
        except OSError as exc:
            log(f"mlsd failed: {exc}")
            self.reply(f, "426 Transfer aborted")
        return True

    def cmd_retr(self, sess: Session, f, arg: str) -> bool:
        try:
            real = self.real_path(self.join(sess.cwd, arg))
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        if not os.path.isfile(real):
            self.reply(f, "550 Not a file")
            return True
        data = self.data_socket(sess, f, arg)
        if data is None:
            return True
        self.reply(f, "150 Opening data connection")
        offset = sess.rest_offset
        sess.rest_offset = 0
        try:
            with data, open(real, "rb") as src:
                if offset:
                    src.seek(offset)
                while True:
                    chunk = src.read(65536)
                    if not chunk:
                        break
                    data.sendall(chunk)
            self.reply(f, "226 Transfer complete")
        except OSError as exc:
            log(f"retr failed: {exc}")
            self.reply(f, "426 Transfer aborted")
        return True

    def cmd_stor(self, sess: Session, f, arg: str, append: bool) -> bool:
        if self.readonly:
            self.reply(f, "550 Read-only server")
            return True
        try:
            real = self.real_path(self.join(sess.cwd, arg))
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        parent = os.path.dirname(real)
        if not os.path.isdir(parent):
            self.reply(f, "550 No such directory")
            return True
        data = self.data_socket(sess, f, arg)
        if data is None:
            return True
        self.reply(f, "150 Opening data connection")
        mode = "ab" if append else "wb"
        try:
            with open(real, mode) as dst:
                while True:
                    chunk = data.recv(65536)
                    if not chunk:
                        break
                    dst.write(chunk)
            self.reply(f, "226 Transfer complete")
        except OSError as exc:
            log(f"stor failed: {exc}")
            self.reply(f, "426 Transfer aborted")
        finally:
            try:
                data.close()
            except OSError:
                pass
        return True

    def cmd_dele(self, sess: Session, f, arg: str) -> bool:
        if self.readonly:
            self.reply(f, "550 Read-only server")
            return True
        try:
            real = self.real_path(self.join(sess.cwd, arg))
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        if not os.path.isfile(real):
            self.reply(f, "550 Not a file")
            return True
        try:
            os.remove(real)
            self.reply(f, "250 Deleted")
        except OSError as exc:
            self.reply(f, "550 Delete failed: %s" % exc)
        return True

    def cmd_mkd(self, sess: Session, f, arg: str) -> bool:
        if self.readonly:
            self.reply(f, "550 Read-only server")
            return True
        try:
            real = self.real_path(self.join(sess.cwd, arg))
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        try:
            os.mkdir(real)
            self.reply(f, '257 "%s" created' % self.virtual_of(real, self.root))
        except FileExistsError:
            self.reply(f, "550 Already exists")
        except OSError as exc:
            self.reply(f, "550 Cannot create: %s" % exc)
        return True

    def cmd_rmd(self, sess: Session, f, arg: str) -> bool:
        if self.readonly:
            self.reply(f, "550 Read-only server")
            return True
        try:
            real = self.real_path(self.join(sess.cwd, arg))
        except PermissionError:
            self.reply(f, "550 Invalid path")
            return True
        if real == self.root:
            self.reply(f, "550 Cannot remove root")
            return True
        try:
            os.rmdir(real)
            self.reply(f, "250 Removed")
        except OSError as exc:
            self.reply(f, "550 Remove failed: %s" % exc)
        return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Local FTP server for POS Commander tests")
    ap.add_argument("--port", type=int, default=2121)
    ap.add_argument("--user", default="test")
    ap.add_argument("--password", default="test")
    ap.add_argument("--root", default="", help="root directory (temp dir if empty)")
    ap.add_argument("--anonymous", action="store_true")
    ap.add_argument("--readonly", action="store_true")
    ap.add_argument("--mlsd", action="store_true", help="advertise and serve MLSD")
    ap.add_argument("--keep", action="store_true", help="do not delete temp root on exit")
    args = ap.parse_args()

    root = args.root or tempfile.mkdtemp(prefix="posc2-ftp-")
    os.makedirs(root, exist_ok=True)
    server = FtpTestServer(root, args.user, args.password, args.anonymous, args.readonly, args.port, args.mlsd)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
        if not args.root and not args.keep:
            import shutil
            shutil.rmtree(root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
