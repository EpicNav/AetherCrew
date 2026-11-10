"""Direct API regression tests. All HTTP traffic terminates at a local fake server."""
import importlib.util
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "Resources/Scripts/hyperai_api_agent.py"
spec = importlib.util.spec_from_file_location("api_agent", SCRIPT)
api = importlib.util.module_from_spec(spec)
spec.loader.exec_module(api)


def response(provider, text="Hello from the API", tool=False):
    if provider == "anthropic":
        return {"id": "message-1", "content": [{"type": "tool_use", "id": "call-1", "name": "read_file", "input": {"path": "note.txt"}}]
                if tool else [{"type": "text", "text": text}], "stop_reason": "tool_use" if tool else "end_turn"}
    if provider == "openai":
        return {"id": "response-1", "status": "completed", "output": [{"type": "function_call", "call_id": "call-1", "name": "read_file", "arguments": '{"path":"note.txt"}'}]
                if tool else [{"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": text}]}]}
    return {"candidates": [{"finishReason": "STOP", "content": {"role": "model", "parts":
            [{"functionCall": {"id": "call-1", "name": "read_file", "args": {"path": "note.txt"}}, "thoughtSignature": "signature-must-survive"}]
            if tool else [{"text": text}]}}]}


class Endpoint:
    def __init__(self, callback):
        self.calls = []
        outer = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.do_POST()

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"]))) if self.command == "POST" else None
                outer.calls.append((self.path, body, {k.lower(): v for k, v in self.headers.items()}))
                answer = callback(self.path, body, len(outer.calls))
                status, result, headers = answer if isinstance(answer, tuple) else (200, answer, {})
                data = result.encode() if isinstance(result, str) else json.dumps(result).encode()
                self.send_response(status)
                self.send_header("Content-Type", headers.get("Content-Type", "application/json"))
                self.send_header("Content-Length", str(len(data)))
                for key, value in headers.items():
                    if key != "Content-Type":
                        self.send_header(key, value)
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:" + str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)


class DirectApiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "note.txt").write_text("A real tool result", encoding="utf-8")
        self.events = []
        self.environment = patch.dict(os.environ, {"ANTHROPIC_API_KEY": "fake-anthropic-secret-0123",
            "OPENAI_API_KEY": "fake-openai-secret-0123", "GEMINI_API_KEY": "fake-gemini-secret-0123"})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def agent(self, provider, endpoint, **kwargs):
        _, _, variable, _, _ = api.PROVIDERS[provider]
        os.environ[variable] = endpoint.url
        return api.ApiAgent(provider, self.root, emit=self.events.append, **kwargs)

    def test_real_generation_requests_for_all_providers_no_balance_checks(self):
        for provider in api.PROVIDERS:
            with self.subTest(provider=provider), Endpoint(lambda *args: response(provider)) as endpoint:
                agent = self.agent(provider, endpoint)
                agent.turn("Hello, Claude — bonjour 日本語")
                self.assertFalse(self.events[-1]["is_error"])
                self.assertEqual(len(endpoint.calls), 1)
                path, body, headers = endpoint.calls[0]
                expected = {"anthropic": "/v1/messages", "openai": "/responses", "gemini": "/v1beta/models/gemini-3.8-flash:generateContent"}
                self.assertEqual(path, expected[provider])
                self.assertNotIn("models?", path)
                self.assertNotIn("balance", path)
                if provider == "anthropic":
                    self.assertEqual(body["model"], "claude-opus-5-5")
                    self.assertEqual(headers["x-api-key"], os.environ["ANTHROPIC_API_KEY"])
                    self.assertEqual(headers["anthropic-version"], "2023-06-01")
                    self.assertEqual(body["messages"][0]["content"], [{"type": "text", "text": "Hello, Claude — bonjour 日本語"}])

    def test_tool_loop_and_provider_native_result_formats(self):
        for provider in api.PROVIDERS:
            with self.subTest(provider=provider), Endpoint(lambda path, body, n: response(provider, tool=n == 1)) as endpoint:
                agent = self.agent(provider, endpoint)
                agent.turn("Read note.txt, then answer")
                self.assertEqual(len(endpoint.calls), 2)
                self.assertFalse(self.events[-1]["is_error"])
                request = endpoint.calls[-1][1]
                if provider == "anthropic":
                    result = request["messages"][-1]["content"][0]
                    self.assertEqual(result["tool_use_id"], "call-1")
                    self.assertIn("A real tool result", result["content"])
                elif provider == "openai":
                    self.assertEqual(request["input"][-1]["type"], "function_call_output")
                    self.assertEqual(request["input"][-1]["call_id"], "call-1")
                    self.assertFalse(request["store"])
                else:
                    self.assertEqual(request["contents"][-2]["parts"][0]["thoughtSignature"], "signature-must-survive")
                    self.assertEqual(request["contents"][-1]["parts"][0]["functionResponse"]["id"], "call-1")

    def test_history_resume_and_no_credentials_on_disk(self):
        with Endpoint(lambda *args: response("anthropic")) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.turn("Remember the red cube")
            resumed = self.agent("anthropic", endpoint, session=agent.session)
            resumed.turn("What colour was it?")
            self.assertEqual(len(endpoint.calls[-1][1]["messages"]), 3)
            for path in agent.folder.iterdir():
                text = path.read_text(encoding="utf-8")
                self.assertNotIn(os.environ["ANTHROPIC_API_KEY"], text)
                self.assertNotIn('"x-api-key"', text)

    def test_api_error_is_visible_redacted_and_not_retried(self):
        key = os.environ["ANTHROPIC_API_KEY"]
        with Endpoint(lambda *args: (401, {"error": {"message": "Invalid " + key}}, {})) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.turn("Hello")
            self.assertTrue(self.events[-1]["is_error"])
            self.assertIn("HTTP 401", self.events[-1]["result"])
            self.assertNotIn(key, json.dumps(self.events))
            self.assertEqual(len(endpoint.calls), 1)

    def test_missing_key_does_not_send_any_request(self):
        with Endpoint(lambda *args: {}) as endpoint, patch.dict(os.environ, {"ANTHROPIC_API_KEY": ""}):
            with self.assertRaisesRegex(ValueError, "ANTHROPIC_API_KEY"):
                self.agent("anthropic", endpoint)
            self.assertEqual(endpoint.calls, [])

    def test_explicit_model_and_image_payload(self):
        for provider in api.PROVIDERS:
            with self.subTest(provider=provider), Endpoint(lambda *args: response(provider)) as endpoint:
                agent = self.agent(provider, endpoint, model="my-exact-model", max_output_tokens=8192)
                agent.turn([{"type": "text", "text": "Describe"}, {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "aGVsbG8="}}])
                path, body, _ = endpoint.calls[0]
                self.assertEqual(body.get("model", "my-exact-model"), "my-exact-model")
                self.assertIn("aGVsbG8=", json.dumps(body))
                limit = body.get("max_tokens", body.get("max_output_tokens", body.get("generationConfig", {}).get("maxOutputTokens")))
                self.assertEqual(limit, 8192)
                if provider == "gemini":
                    self.assertIn("my-exact-model", path)

    def test_auto_and_shared_output_settings_apply_on_next_request(self):
        for provider in api.PROVIDERS:
            with self.subTest(provider=provider), Endpoint(lambda *args: response(provider)) as endpoint:
                api.set_output_tokens(self.root, 0)
                agent = self.agent(provider, endpoint)
                agent.turn("Auto")
                body = endpoint.calls[-1][1]
                if provider == "anthropic":
                    self.assertEqual(body["max_tokens"], 128000)
                    self.assertTrue(body["stream"])
                else:
                    self.assertNotIn("max_output_tokens", body)
                    self.assertNotIn("generationConfig", body)
                api.set_output_tokens(self.root, 24000)
                agent.turn("Changed without restart")
                body = endpoint.calls[-1][1]
                limit = body.get("max_tokens", body.get("max_output_tokens", body.get("generationConfig", {}).get("maxOutputTokens")))
                self.assertEqual(limit, 24000)
                self.assertFalse(self.events[-1]["is_error"])
        for invalid in (-1, True, 1.5, "unlimited"):
            with self.assertRaises(ValueError):
                api.set_output_tokens(self.root, invalid)
        (self.root / api.OPTIONS_FILE).write_text('{"max_output_tokens":-2}')
        with self.assertRaisesRegex(ValueError, "Invalid API output"):
            api.output_tokens(self.root)

    def test_unknown_claude_model_auto_discovers_output_limit_once(self):
        with Endpoint(lambda path, body, n: {"max_tokens": 48000} if body is None else response("anthropic")) as endpoint:
            agent = self.agent("anthropic", endpoint, model="custom-claude")
            agent.turn("Hello")
            agent.turn("Again")
            self.assertEqual([c[0] for c in endpoint.calls], ["/v1/models/custom-claude", "/v1/messages", "/v1/messages"])
            self.assertEqual(endpoint.calls[-1][1]["max_tokens"], 48000)
            self.assertFalse(self.events[-1]["is_error"])

    @staticmethod
    def stream_response(tool=False, stop="end_turn", complete=True, invalid_json=False):
        block = {"type": "tool_use", "id": "call-stream", "name": "read_file", "input": {}} if tool else {"type": "text", "text": ""}
        events = [
            {"type": "message_start", "message": {"id": "stream-1", "role": "assistant", "content": [], "usage": {"input_tokens": 12}}},
            {"type": "content_block_start", "index": 0, "content_block": block},
            {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": '{"path":' if invalid_json else '{"path":"note.txt"}'}
             if tool else {"type": "text_delta", "text": "Streamed answer"}},
            {"type": "content_block_stop", "index": 0},
            {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 4}}]
        if complete:
            events.append({"type": "message_stop"})
        return 200, "".join("data: " + json.dumps(event) + "\n\n" for event in events), {"Content-Type": "text/event-stream"}

    def test_claude_sse_stream_text_and_tool_results(self):
        with Endpoint(lambda path, body, n: self.stream_response(tool=n == 1, stop="tool_use" if n == 1 else "end_turn")) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.turn("Read the file")
            self.assertEqual(len(endpoint.calls), 2)
            self.assertIn("A real tool result", endpoint.calls[-1][1]["messages"][-1]["content"][0]["content"])
            self.assertFalse(self.events[-1]["is_error"])
            self.assertEqual(agent.history[-1]["content"][0]["text"], "Streamed answer")
            self.assertTrue(any(e.get("type") == "stream_event" for e in self.events))

    def test_stream_disconnect_and_token_limit_never_run_incomplete_tools(self):
        for complete, stop, invalid in [(False, "tool_use", False), (True, "max_tokens", True)]:
            with self.subTest(complete=complete), Endpoint(lambda *args: self.stream_response(tool=True, stop=stop, complete=complete, invalid_json=invalid)) as endpoint:
                agent = self.agent("anthropic", endpoint)
                with patch.object(agent, "tool") as tool:
                    agent.turn("Run a tool")
                    tool.assert_not_called()
                self.assertTrue(self.events[-1]["is_error"])
                if complete:
                    self.assertEqual(agent.history[-1]["content"][0]["type"], "tool_result")
                    self.assertTrue(agent.history[-1]["content"][0]["is_error"])
        with Endpoint(lambda *args: self.stream_response(stop="max_tokens")) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.turn("Long response")
            self.assertTrue(self.events[-1]["is_error"])
            self.assertEqual(agent.history[-1]["content"][0]["text"], "Streamed answer")

    def test_terminal_actual_process_shared_settings_approvals_and_resume(self):
        def reply(path, body, n):
            if n == 1:
                return {"id": "write", "content": [{"type": "tool_use", "id": "write-1", "name": "write_file", "input": {"path": "terminal-denied.txt", "content": "no"}}], "stop_reason": "tool_use"}
            self.assertTrue(body["messages"][-1]["content"][0]["is_error"])
            return response("anthropic", text="The write was denied")

        with Endpoint(reply) as endpoint:
            os.environ["ANTHROPIC_BASE_URL"] = endpoint.url
            session = "terminal-session"
            command = [sys.executable, str(SCRIPT), "--provider", "anthropic", "--root", str(self.root), "--session", session, "--terminal"]
            result = subprocess.run(command, input="/tokens 8192\nWrite a file\nn\n/tokens 0\n/exit\n", text=True, encoding="utf-8", capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Allow this action?", result.stdout)
            self.assertIn("The write was denied", result.stdout)
            self.assertNotIn(os.environ["ANTHROPIC_API_KEY"], result.stdout)
            self.assertFalse((self.root / "terminal-denied.txt").exists())
            self.assertEqual(endpoint.calls[0][1]["max_tokens"], 8192)
            self.assertEqual(api.output_tokens(self.root), 0)
            resumed = self.agent("anthropic", endpoint, session=session)
            self.assertEqual(resumed.history[-1]["content"][0]["text"], "The write was denied")
            replay = subprocess.run(command, input="/exit\n", text=True, encoding="utf-8", capture_output=True, timeout=20)
            self.assertIn("The write was denied", replay.stdout)
            self.assertEqual(len(endpoint.calls), 2)

    def test_terminal_initial_manifest_and_stream_text_not_duplicated(self):
        manifest = self.root / "initial.json"
        manifest.write_text(json.dumps({"text": "Opening prompt", "images": []}))
        with Endpoint(lambda *args: self.stream_response()) as endpoint:
            os.environ["ANTHROPIC_BASE_URL"] = endpoint.url
            command = [sys.executable, str(SCRIPT), "--provider", "anthropic", "--root", str(self.root), "--terminal", "--prompt-file", str(manifest)]
            result = subprocess.run(command, input="/exit\n", text=True, encoding="utf-8", capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.count("Streamed answer"), 1)
            self.assertEqual(endpoint.calls[0][1]["messages"][0]["content"][0]["text"], "Opening prompt")

    def test_terminal_interrupt_preserves_resumable_tool_history(self):
        with Endpoint(lambda *args: response("anthropic", tool=True)) as endpoint:
            agent = self.agent("anthropic", endpoint)
            with patch.object(agent, "tool", side_effect=KeyboardInterrupt):
                agent.turn("Read file")
            self.assertTrue(agent.cancel.is_set())
            self.assertTrue(self.events[-1]["is_error"])
            self.assertEqual(agent.history[-1]["content"][0]["tool_use_id"], "call-1")
            self.assertTrue(agent.history[-1]["content"][0]["is_error"])

    def test_files_are_scoped_and_mutation_needs_approval(self):
        with Endpoint(lambda *args: {}) as endpoint:
            agent = self.agent("anthropic", endpoint, approval=lambda *args: False)
            with self.assertRaises(PermissionError):
                agent.tool({"id": "x", "name": "write_file", "input": {"path": "denied.txt", "content": "wrong"}})
            with self.assertRaises(PermissionError):
                agent.tool({"id": "x", "name": "run_command", "input": {"command": "echo must-not-run"}})
            self.assertFalse((self.root / "denied.txt").exists())
            with self.assertRaises(ValueError):
                agent.tool({"id": "x", "name": "read_file", "input": {"path": "../outside.txt"}})
            agent.approval = lambda *args: True
            agent.tool({"id": "x", "name": "write_file", "input": {"path": "allowed.txt", "content": "approved"}})
            self.assertEqual((self.root / "allowed.txt").read_text(), "approved")
            agent.read_only = True
            with self.assertRaises(PermissionError):
                agent.tool({"id": "x", "name": "write_file", "input": {"path": "other.txt", "content": "wrong"}})

    def test_mcp_discovery_execution_and_approval(self):
        mutations, approvals = [], []

        def mcp(path, body, n):
            method = body["method"]
            if method == "initialize":
                result = {"protocolVersion": "2025-03-26", "capabilities": {"tools": {}}}
            elif method == "notifications/initialized":
                return 202, {}, {}
            elif method == "tools/list":
                self.assertEqual(body["params"], {})
                result = {"tools": [{"name": "edit_world", "inputSchema": api.schema({})},
                                     {"name": "inspect_world", "inputSchema": api.schema({}), "annotations": {"readOnlyHint": True}}]}
            else:
                self.assertEqual(method, "tools/call")
                mutations.append(body["params"]["name"])
                result = {"content": [{"type": "text", "text": "Done"}]}
            return 200, {"jsonrpc": "2.0", "id": body.get("id"), "result": result}, {"Mcp-Session-Id": "test-session"}

        with Endpoint(mcp) as endpoint:
            agent = self.agent("anthropic", endpoint, mcp=endpoint.url + "/mcp",
                               approval=lambda *args: approvals.append(args) or False)
            found = agent.tool({"id": "x", "name": "unreal_list_tools", "input": {"query": "world"}})
            self.assertEqual(len(found["tools"]), 2)
            with self.assertRaises(PermissionError):
                agent.tool({"id": "x", "name": "unreal_call_tool", "input": {"name": "edit_world", "arguments": {}}})
            agent.tool({"id": "x", "name": "unreal_call_tool", "input": {"name": "inspect_world", "arguments": {}}})
            self.assertEqual(mutations, ["inspect_world"])
            self.assertEqual(len(approvals), 1)
            self.assertTrue(all("x-api-key" not in headers and "authorization" not in headers for _, _, headers in endpoint.calls))
            self.assertEqual(endpoint.calls[-1][2]["mcp-session-id"], "test-session")

    def test_cancellation_prevents_api_and_tools(self):
        with Endpoint(lambda *args: response("anthropic")) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.cancel.set()
            agent.turn("Do not send this")
            self.assertTrue(self.events[-1]["is_error"])
            self.assertEqual(endpoint.calls, [])
            with self.assertRaises(api.Cancelled):
                agent.tool({"id": "x", "name": "read_file", "input": {"path": "note.txt"}})

    def test_redirect_does_not_forward_key(self):
        with Endpoint(lambda *args: (302, {}, {"Location": "https://other.example/v1/messages"})) as endpoint:
            agent = self.agent("anthropic", endpoint)
            agent.turn("Hello")
            self.assertEqual(len(endpoint.calls), 1)
            self.assertIn("redirect refused", self.events[-1]["result"])

    def test_stdio_handshake_generation_and_editor_approval(self):
        def callback(path, body, n):
            result = response("anthropic", tool=n == 1)
            if n == 1:
                result["content"][0].update(name="write_file", input={"path": "stdio-denied.txt", "content": "not approved"})
            else:
                self.assertTrue(body["messages"][-1]["content"][0]["is_error"])
            return result

        with Endpoint(callback) as endpoint:
            env = os.environ.copy()
            env["ANTHROPIC_BASE_URL"] = endpoint.url
            process = subprocess.Popen([sys.executable, "-u", str(SCRIPT), "--provider", "anthropic", "--root", str(self.root)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)
            outputs = queue.Queue()
            def read():
                for line in process.stdout:
                    outputs.put(json.loads(line))
            reader = threading.Thread(target=read, daemon=True)
            reader.start()
            def send(value):
                process.stdin.write(json.dumps(value) + "\n")
                process.stdin.flush()
            try:
                send({"type": "control_request", "request_id": "hyperai-initialize", "request": {"subtype": "initialize"}})
                self.assertEqual(outputs.get(timeout=10)["response"]["subtype"], "success")
                self.assertEqual(outputs.get(timeout=10)["type"], "system")
                self.assertEqual(endpoint.calls, [])
                send({"type": "user", "message": {"role": "user", "content": "Hello, Claude 日本語"}})
                for _ in range(12):
                    message = outputs.get(timeout=10)
                    if message["type"] == "control_request":
                        send({"type": "control_response", "response": {"request_id": message["request_id"],
                             "subtype": "success", "response": {"behavior": "deny"}}})
                    if message["type"] == "result":
                        self.assertFalse(message["is_error"])
                        break
                else:
                    self.fail("No final API result")
                self.assertEqual(len(endpoint.calls), 2)
                self.assertFalse((self.root / "stdio-denied.txt").exists())
            finally:
                process.stdin.close()
                process.wait(timeout=5)
                process.stdout.close()
                process.stderr.close()
                reader.join(timeout=2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
