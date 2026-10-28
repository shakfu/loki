"""A scripted ACP agent for test_acp.c. The prompt text selects a script.

  hello            stream two chunks
  info             report the initialize and session/new params
  edit PATH        read PATH, write it back with 1 replaced by 2
  create PATH      write a new file
  escape           try to read /etc/hostname (must be refused)
  perm             ask permission, report the outcome
  wait             ask permission, then expect session/cancel
"""
import json, sys

pending = {}      # id -> response
cancelled = False
next_id = 100
seen = {}


def send(msg):
    msg["jsonrpc"] = "2.0"
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def read():
    global cancelled
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    msg = json.loads(line)
    if msg.get("method") == "session/cancel":
        cancelled = True
        return None
    return msg


def call(method, params):
    """Send a request to the client and wait for its response."""
    global next_id
    next_id += 1
    send({"id": next_id, "method": method, "params": params})
    while next_id not in pending:
        msg = read()
        if msg is not None and "id" in msg and "method" not in msg:
            pending[msg["id"]] = msg
    return pending.pop(next_id)


def chunk(sid, text):
    send({"method": "session/update", "params": {"sessionId": sid, "update": {
        "sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": text}}}})


def perm(sid):
    return call("session/request_permission", {"sessionId": sid,
        "toolCall": {"toolCallId": "c1", "title": "Run tests"},
        "options": [{"optionId": "allow-once", "name": "Allow once", "kind": "allow_once"},
                    {"optionId": "reject-once", "name": "Reject", "kind": "reject_once"}]})


def prompt(msg):
    p = msg["params"]
    sid = p["sessionId"]
    text = p["prompt"][0]["text"]
    word, _, arg = text.partition(" ")
    stop = "end_turn"
    if word == "hello":
        chunk(sid, "Hello, ")
        chunk(sid, "world")
    elif word == "info":
        chunk(sid, json.dumps({"init": seen["initialize"], "new": seen["session/new"],
                               "prompt": p["prompt"]}))
    elif word == "edit":
        r = call("fs/read_text_file", {"sessionId": sid, "path": arg})
        content = r["result"]["content"]
        call("fs/write_text_file", {"sessionId": sid, "path": arg,
                                    "content": content.replace("1", "2")})
        part = call("fs/read_text_file", {"sessionId": sid, "path": arg, "line": 2, "limit": 1})
        chunk(sid, "line2=" + part["result"]["content"])
    elif word == "create":
        r = call("fs/write_text_file", {"sessionId": sid, "path": arg, "content": "new\n"})
        chunk(sid, "created" if "result" in r else "failed")
    elif word == "escape":
        r = call("fs/read_text_file", {"sessionId": sid, "path": "/etc/hostname"})
        chunk(sid, "denied" if "error" in r else "LEAKED")
    elif word == "perm":
        o = perm(sid)["result"]["outcome"]
        chunk(sid, "chose:" + o.get("optionId", o["outcome"]))
    elif word == "wait":
        o = perm(sid)["result"]["outcome"]
        while not cancelled:
            read()
        chunk(sid, "outcome:" + o["outcome"])
        stop = "cancelled"
    send({"id": msg["id"], "result": {"stopReason": stop}})


def main():
    while True:
        msg = read()
        if msg is None or "method" not in msg:
            continue
        m = msg["method"]
        seen[m] = msg.get("params")
        if m == "initialize":
            send({"id": msg["id"], "result": {"protocolVersion": 1, "agentCapabilities": {},
                                               "authMethods": []}})
        elif m == "session/new":
            send({"id": msg["id"], "result": {"sessionId": "s1"}})
        elif m == "session/prompt":
            prompt(msg)


main()
