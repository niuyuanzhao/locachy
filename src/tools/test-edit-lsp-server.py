#!/usr/bin/env python3
"""Deterministic, strict stdio LSP peer; never used by the editor itself."""
import json
import os
import signal
import sys
import time

MODE = sys.argv[1] if len(sys.argv) > 1 else "normal"
documents = {}
held = []
initialized = False
shutting_down = False
fragment = 0
client_capabilities = {}


def send(message):
    global fragment
    body = json.dumps({"jsonrpc": "2.0", **message}, ensure_ascii=False).encode()
    frame = f"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncOnTeNt-LeNgTh: {len(body)}\r\n\r\n".encode() + body
    # Vary every boundary, including multibyte UTF-8 and header delimiter.
    while frame:
        n = [1, 2, 7, 3, 31, 5, 128][fragment % 7]
        os.write(1, frame[:n])
        frame = frame[n:]
        fragment += 1
        if MODE == "slow-fragments":
            time.sleep(0.0001)


def reply(request, result):
    send({"id": request["id"], "result": result})


def read():
    size = None
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            raise EOFError
        if line == b"\r\n":
            break
        key, value = line.split(b":", 1)
        if key.lower() == b"content-length":
            size = int(value)
    assert size is not None
    return json.loads(sys.stdin.buffer.read(size))


def rng(start=0, end=1, line=0):
    return {"start": {"line": line, "character": start}, "end": {"line": line, "character": end}}


def edit(uri, version=None):
    return {"documentChanges": [{"textDocument": {"uri": uri, "version": version}, "edits": [
        {"range": rng(), "newText": "renamed", "annotationId": "a"}]}],
        "changeAnnotations": {"a": {"label": "Rename symbol", "needsConfirmation": True}}}


def diagnostics(uri, version):
    send({"method": "textDocument/publishDiagnostics", "params": {
        "uri": uri, "version": version, "diagnostics": [{"range": rng(), "message": "deterministic warning 世界",
        "severity": 2, "code": 42, "source": "fake", "tags": [1], "data": {"fix": 1},
        "relatedInformation": [{"location": {"uri": uri, "range": rng()}, "message": "related"}]}]}})


def main():
    global initialized, shutting_down, client_capabilities
    while True:
        request = read()
        method = request.get("method")
        params = request.get("params", {})
        if method is None:
            if request["id"] == "apply-test":
                assert request["result"]["applied"] is True
                send({"method": "window/logMessage", "params": {"type": 3, "message": "apply acknowledged"}})
            elif request["id"] == "apply-timeout":
                assert request["result"]["applied"] is False
                send({"method": "window/logMessage", "params": {"type": 3, "message": "apply expired"}})
            elif request["id"] == "apply-resources":
                result = request["result"]
                assert result == {"applied": False, "failureReason": "rolled back: test"}, result
                send({"method": "window/logMessage", "params": {"type": 3, "message": "apply-resources failed: " + result["failureReason"]}})
            elif request["id"] == "configuration-test":
                assert request["result"] == [{"flag": True}, None]
            continue
        if method == "initialize":
            assert params["capabilities"]["general"]["positionEncodings"] == ["utf-16"]
            client_capabilities = params["capabilities"]
            if MODE == "init-hang":
                continue
            if MODE == "oversize":
                os.write(1, b"Content-Length: 99999999999\r\n\r\n")
                continue
            if MODE == "malformed":
                os.write(1, b"Content-Length: 1\r\n\r\n{")
                continue
            if MODE == "duplicate-header":
                os.write(1, b"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}")
                continue
            if MODE == "partial-crash":
                os.write(1, b"Content-Length: 90\r\n\r\n{\"jsonrpc\":\"2.0\"")
                os._exit(9)
            if MODE == "nul":
                send({"id": request["id"], "result": {"x": "bad\x00text"}})
                continue
            caps = {"positionEncoding": "utf-16", "textDocumentSync": {"openClose": True, "change": 1 if MODE == "full" else 2, "save": {"includeText": True}},
                    "completionProvider": {"triggerCharacters": [".", ":"], "resolveProvider": True},
                    "signatureHelpProvider": {"triggerCharacters": ["("], "retriggerCharacters": [","]},
                    "hoverProvider": True, "definitionProvider": True, "declarationProvider": True,
                    "typeDefinitionProvider": True, "implementationProvider": True, "referencesProvider": True,
                    "documentSymbolProvider": True, "workspaceSymbolProvider": True, "documentHighlightProvider": True,
                    "codeActionProvider": {"resolveProvider": True}, "renameProvider": {"prepareProvider": True},
                    "documentFormattingProvider": True, "documentRangeFormattingProvider": True,
                    "documentOnTypeFormattingProvider": {"firstTriggerCharacter": "}", "moreTriggerCharacter": [";"]}}
            reply(request, {"capabilities": caps, "serverInfo": {"name": "deterministic", "version": "1"}})
        elif method == "initialized":
            initialized = True
            # More than pipe capacity: client must drain this without blocking stdout.
            os.write(2, b"server stderr test\n" * 12000)
            send({"id": "configuration-test", "method": "workspace/configuration", "params": {"items": [{"section": "test"}, {"section": "missing"}]}})
            if MODE == "flood":
                for _ in range(1300):
                    send({"method": "window/logMessage", "params": {"type": 3, "message": "flood"}})
        elif method == "workspace/didChangeConfiguration":
            assert params["settings"]["test"]["flag"] is True
        elif method == "textDocument/didOpen":
            assert initialized
            td = params["textDocument"]
            assert td["uri"] not in documents
            documents[td["uri"]] = td.copy()
            diagnostics(td["uri"], td["version"])
        elif method == "textDocument/didChange":
            td = params["textDocument"]
            d = documents[td["uri"]]
            assert td["version"] > d["version"]
            for change in params["contentChanges"]:
                if MODE == "full":
                    assert "range" not in change
                    d["text"] = change["text"]
                else:
                    r = change["range"]
                    encoded = d["text"].encode("utf-16-le")
                    lines = d["text"].splitlines(keepends=True)
                    def offset(p):
                        return len("".join(lines[:p["line"]]).encode("utf-16-le")) + p["character"] * 2
                    a, b = offset(r["start"]), offset(r["end"])
                    d["text"] = (encoded[:a] + change["text"].encode("utf-16-le") + encoded[b:]).decode("utf-16-le")
            diagnostics(td["uri"], d["version"])  # Deliberately stale.
            d["version"] = td["version"]
            diagnostics(td["uri"], d["version"])
        elif method == "textDocument/didSave":
            d = documents[params["textDocument"]["uri"]]
            assert params["text"] == d["text"]
            send({"method": "window/logMessage", "params": {"type": 3, "message": "saved " + d["text"]}})
        elif method == "textDocument/didClose":
            uri = params["textDocument"]["uri"]
            assert documents.pop(uri)
            diagnostics(uri, 1)  # Closed document diagnostics must be ignored.
        elif method == "$/cancelRequest":
            # Legal late reply after cancellation: the client must ignore it.
            for r in held:
                if r["id"] == params["id"]:
                    reply(r, {"contents": "late reply"})
            held[:] = [r for r in held if r["id"] != params["id"]]
        elif method == "shutdown":
            assert params is None
            shutting_down = True
            if MODE != "shutdown-hang":
                reply(request, None)
            else:
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
        elif method == "exit":
            assert shutting_down
            return
        elif method == "workspace/executeCommand":
            command = params["command"]
            if command == "crash":
                os._exit(23)
            if command == "hold":
                held.append(request)
            elif command == "apply-timeout":
                send({"id": "apply-timeout", "method": "workspace/applyEdit", "params": {"label": "Expire", "edit": edit(next(iter(documents)))}})
                reply(request, None)
            elif command == "capabilities":
                send({"method": "window/logMessage", "params": {"type": 3, "message": json.dumps(client_capabilities["workspace"], sort_keys=True)}})
                reply(request, None)
            elif command == "apply-resources":
                # Every CreateFile/RenameFile/DeleteFile option, and malformed ones.
                uri = next(iter(documents))
                changes = [
                    {"kind": "create", "uri": "file:///tmp/c1.c"},
                    {"kind": "create", "uri": "file:///tmp/c2.c", "options": {"overwrite": True, "ignoreIfExists": True}, "annotationId": "a"},
                    {"kind": "create", "uri": "file:///tmp/c3.c", "options": {"ignoreIfExists": True}},
                    {"textDocument": {"uri": "file:///tmp/c1.c", "version": None}, "edits": [{"range": rng(0, 0), "newText": "x"}]},
                    {"kind": "rename", "oldUri": "file:///tmp/r1.c", "newUri": "file:///tmp/r2.c"},
                    {"kind": "rename", "oldUri": "file:///tmp/r3.c", "newUri": "file:///tmp/r4.c", "options": {"overwrite": False, "ignoreIfExists": True}},
                    {"kind": "delete", "uri": "file:///tmp/d1", "options": {"recursive": True}},
                    {"kind": "delete", "uri": "file:///tmp/d2.c", "options": {"ignoreIfNotExists": True}},
                    {"kind": "create", "uri": "file:///tmp/bad.c", "options": {"overwrite": "yes"}},
                    {"kind": "rename", "newUri": "file:///tmp/bad.c"},
                    {"kind": "delete"},
                    {"kind": "delete", "uri": "file:///tmp/bad.c", "options": [True]},
                    {"kind": "move", "uri": "file:///tmp/bad.c"},
                    {"textDocument": {"uri": uri, "version": documents[uri]["version"]}, "edits": [{"range": rng(), "newText": "renamed"}]}]
                send({"id": "apply-resources", "method": "workspace/applyEdit", "params": {"label": "Resources", "edit": {
                    "documentChanges": changes, "changeAnnotations": {"a": {"label": "Create c2", "needsConfirmation": False}}}}})
                reply(request, None)
            elif command == "apply":
                uri = next(iter(documents))
                value = edit(uri, documents[uri]["version"])
                value["documentChanges"] += [
                    {"kind": "create", "uri": "file:///tmp/new.c", "options": {"ignoreIfExists": True}},
                    {"kind": "rename", "oldUri": "file:///tmp/a.c", "newUri": "file:///tmp/b.c", "options": {"overwrite": True}},
                    {"kind": "delete", "uri": "file:///tmp/old.c", "options": {"recursive": True, "ignoreIfNotExists": True}}]
                send({"id": "apply-test", "method": "workspace/applyEdit", "params": {"label": "Transaction", "edit": value}})
                reply(request, None)
            else:
                send({"id": request["id"], "error": {"code": -32602, "message": "Unknown command"}})
        elif method == "workspace/symbol":
            uri = next(iter(documents))
            reply(request, [{"name": "alpha", "kind": 12, "location": {"uri": uri, "range": rng()}}])
        elif method == "completionItem/resolve":
            assert params["data"] == {"token": 3}
            reply(request, {**params, "documentation": {"kind": "markdown", "value": "Resolved **help**"}})
        elif method == "codeAction/resolve":
            assert params["data"] == {"fix": 1}
            uri = next(iter(documents))
            reply(request, {**params, "edit": edit(uri)})
        else:
            uri = params["textDocument"]["uri"]
            assert uri in documents
            d = documents[uri]
            if method == "textDocument/completion":
                reply(request, {"isIncomplete": True, "items": [{"label": "alpha", "kind": 3, "detail": "function", "insertTextFormat": 1,
                    "textEdit": {"insert": rng(0, 0), "replace": rng(), "newText": "alpha"}, "additionalTextEdits": [{"range": rng(0, 0), "newText": "prefix"}], "data": {"token": 3}}]})
            elif method == "textDocument/signatureHelp":
                reply(request, {"activeSignature": 0, "activeParameter": 1, "signatures": [{"label": "alpha(int a, int b)", "documentation": "signature", "parameters": [{"label": [6, 11]}, {"label": "int b"}]}]})
            elif method == "textDocument/hover":
                reply(request, {"contents": {"kind": "markdown", "value": "**alpha** 世界"}, "range": rng()})
            elif method in ("textDocument/definition", "textDocument/declaration", "textDocument/typeDefinition", "textDocument/implementation"):
                reply(request, [{"targetUri": uri, "targetRange": rng(), "targetSelectionRange": rng(), "originSelectionRange": rng()}])
            elif method == "textDocument/references":
                assert params["context"]["includeDeclaration"] is True
                reply(request, [{"uri": u, "range": rng()} for u in documents])
            elif method == "textDocument/documentSymbol":
                reply(request, [{"name": "alpha", "kind": 12, "range": rng(), "selectionRange": rng(), "children": [{"name": "a", "kind": 13, "range": rng(), "selectionRange": rng()}]}])
            elif method == "textDocument/documentHighlight":
                reply(request, [{"range": rng(), "kind": 3}])
            elif method == "textDocument/codeAction":
                assert params["context"]["diagnostics"][0]["data"] == {"fix": 1}
                assert params["context"]["only"] == ["quickfix"]
                reply(request, [{"title": "Fix", "kind": "quickfix", "isPreferred": True, "edit": edit(uri, d["version"]), "data": {"fix": 1}, "command": {"command": "fix", "arguments": [1]}}])
            elif method == "textDocument/rename":
                assert params["newName"] == "renamed"
                reply(request, edit(uri, d["version"]))
            elif method == "textDocument/prepareRename":
                reply(request, {"range": rng(), "placeholder": "alpha"})
            elif method in ("textDocument/formatting", "textDocument/rangeFormatting", "textDocument/onTypeFormatting"):
                assert params["options"]["tabSize"] == 4
                reply(request, [{"range": rng(), "newText": "formatted"}])
            elif method == "textDocument/switchSourceHeader":
                reply(request, "file:///tmp/header.h")
            else:
                raise AssertionError(method)


try:
    main()
except EOFError:
    pass
