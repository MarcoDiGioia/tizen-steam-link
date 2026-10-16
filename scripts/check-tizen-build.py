#!/usr/bin/env python3
"""Checks on the WASM build that the TV would otherwise catch at run time.

1. ihslib's archive must not contain the backends this project replaces. Emscripten's
   toolchain used to update archives in place, so the object of a removed source stayed in
   libihslib.a and won at link time: ihslib's own UDP backend calls fcntl(F_SETFL) on its
   socket, which Samsung's socket layer answers with abort().
2. The module must not import open (___syscall5). File syscalls from worker threads are
   proxied, synchronously, to the browser main thread; mbedTLS used to read /dev/urandom
   that way for every encrypted control message.

3. Every C function handed to ihslib as a thread or timer callback must take the number of
   parameters its callback type declares. Emscripten fastcomp keeps one function table per
   signature, so a function called through a pointer of another signature is looked up in
   the wrong table and some unrelated function runs instead. ihslib's timer thread worker
   was declared without parameters, and on the TV the timer thread never ran a task.
   ihslib builds with -w, so the C compiler says nothing about these.

The module does import fcntl (___syscall221), from musl's socket() fallback, which only
runs for SOCK_NONBLOCK or SOCK_CLOEXEC; nothing here passes those.

Usage: check-tizen-build.py BUILD_DIR
"""
import json
import os
import re
import sys

REPLACED_BACKENDS = ("ihs_udp_posix", "ihs_thread_sdl")
FORBIDDEN_IMPORTS = {"___syscall5": "open (proxied to the main thread from workers)"}
# Registration function: {argument index: parameter count of the callback type}.
CALLBACK_ARGUMENTS = {
    "IHS_ThreadCreate": {0: 1},  # IHS_ThreadFunction(void *context)
    "IHS_TimerTaskStart": {1: 2, 2: 1},  # IHS_TimerRunFunction(int, void *), IHS_TimerEndFunction(void *)
}


def archive_members(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"{path}: not an ar archive")
    i, longnames, names = 8, b"", []
    while i + 60 <= len(data):
        header = data[i:i + 60]
        name = header[:16].decode().strip()
        size = int(header[48:58].decode().strip())
        body = data[i + 60:i + 60 + size]
        if name == "//":
            longnames = body
        elif name.startswith("/") and name[1:].isdigit():
            start = int(name[1:])
            end = longnames.index(b"/\n", start)
            names.append(longnames[start:end].decode())
        elif name not in ("/", "/SYM64/"):
            names.append(name.rstrip("/"))
        i += 60 + size + (size & 1)
    return names


def leb128(data, i):
    result = shift = 0
    while True:
        byte = data[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, i


def wasm_imports(path):
    with open(path, "rb") as f:
        data = f.read()
    i = 8
    while i < len(data):
        section, i = data[i], i + 1
        size, i = leb128(data, i)
        end = i + size
        if section == 2:
            count, i = leb128(data, i)
            for _ in range(count):
                length, i = leb128(data, i)
                i += length
                length, i = leb128(data, i)
                name = data[i:i + length].decode()
                i += length
                kind, i = data[i], i + 1
                if kind == 0:
                    _, i = leb128(data, i)
                elif kind in (1, 2):
                    if kind == 1:
                        i += 1
                    flags, i = leb128(data, i)
                    _, i = leb128(data, i)
                    if flags & 1:
                        _, i = leb128(data, i)
                elif kind == 3:
                    i += 2
                yield name
            return
        i = end


def call_arguments(text, start):
    """Splits the argument list of the call whose "(" is at text[start]."""
    depth, args, current = 0, [], ""
    for ch in text[start:]:
        if ch == "(":
            depth += 1
            if depth == 1:
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(current.strip())
                return args
        elif ch == "," and depth == 1:
            args.append(current.strip())
            current = ""
            continue
        current += ch
    return args


def parameter_count(params):
    params = params.strip()
    return 0 if params in ("", "void") else params.count(",") + 1


def callback_mismatches(build):
    with open(os.path.join(build, "compile_commands.json")) as f:
        sources = sorted({e["file"] for e in json.load(f) if e["file"].endswith(".c")})
    texts = {}
    for path in sources:
        with open(path, errors="replace") as f:
            texts[path] = re.sub(r"/\*.*?\*/|//[^\n]*", "", f.read(), flags=re.S)
    errors, checked = [], 0
    for path, text in texts.items():
        for registrar, expected in CALLBACK_ARGUMENTS.items():
            for m in re.finditer(r"\b%s\s*\(" % registrar, text):
                args = call_arguments(text, m.end() - 1)
                for index, count in expected.items():
                    if index >= len(args):
                        continue
                    # Drop casts such as (IHS_ThreadFunction *); a declaration's
                    # "IHS_ThreadFunction *function" is not a bare name and is skipped.
                    name = re.sub(r"^(\([^()]*\)\s*)+", "", args[index]).lstrip("&").strip()
                    if not re.fullmatch(r"[A-Za-z_]\w*", name) or name == "NULL":
                        continue
                    checked += 1
                    definition = re.compile(r"\b%s\s*\(([^)]*)\)\s*\{" % name)
                    found = definition.search(text) or next(
                        (d for d in map(definition.search, texts.values()) if d), None)
                    if found is None:
                        errors.append(f"{path}: no definition found for {registrar} callback {name}")
                    elif parameter_count(found.group(1)) != count:
                        errors.append(f"{path}: {registrar} callback {name}({found.group(1).strip()}) "
                                      f"takes {parameter_count(found.group(1))} parameters, "
                                      f"its callback type {count}")
    return errors, checked


def main():
    build = sys.argv[1]
    errors = []
    members = archive_members(os.path.join(build, "libihslib.a"))
    for member in members:
        if member.startswith(REPLACED_BACKENDS):
            errors.append(f"libihslib.a contains {member}, a backend this project replaces")
    wasm = os.path.join(build, "src", "tizen", "tsl.wasm")
    imports = set(wasm_imports(wasm))
    for name, why in FORBIDDEN_IMPORTS.items():
        if name in imports:
            errors.append(f"tsl.wasm imports {name}: {why}")
    mismatches, callbacks = callback_mismatches(build)
    errors += mismatches
    for e in errors:
        print("check-tizen-build: " + e, file=sys.stderr)
    if errors:
        return 1
    print(f"check-tizen-build: ok ({len(members)} ihslib objects, {len(imports)} wasm imports, "
          f"{callbacks} thread and timer callbacks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
