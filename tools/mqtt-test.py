#!/usr/bin/python3
"""Check the MQTT settings in /etc/camwall/camwall.conf step by step: the settings
themselves, name lookup, TCP, TLS with the broker's certificate, the MQTT login,
and the retained state on camwall's topics. Re-runs itself with sudo to read the
config file. Prints no password.

  tools/mqtt-test.py [--set cameras|off] [--wait SECONDS]

  --set   also publish this mode to <topic>/mode/set, as Home Assistant would
  --wait  seconds to collect messages after logging in (default 3)

Uses camwalld's own config reading and checks, so both agree on the settings.
"""

import argparse
import datetime
import importlib.machinery
import importlib.util
import os
import socket
import ssl
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CONF = os.environ.get("CAMWALL_CONF", "/etc/camwall/camwall.conf")   # override for testing


def load_camwalld():
    sys.dont_write_bytecode = True   # no root-owned __pycache__ in the checkout
    path = os.path.join(HERE, "..", "bin", "camwalld")
    loader = importlib.machinery.SourceFileLoader("camwalld", path)
    module = importlib.util.module_from_spec(importlib.util.spec_from_loader("camwalld", loader))
    loader.exec_module(module)
    return module


def step(name, text):
    print(f"{name:<12} {text}", flush=True)


def fail(name, text):
    step(name, f"FAILED: {text}")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--set", choices=("cameras", "off"))
    ap.add_argument("--wait", type=float, default=3)
    args = ap.parse_args()
    if os.geteuid() != 0 and "CAMWALL_CONF" not in os.environ:
        os.execvp("sudo", ["sudo", sys.executable, os.path.abspath(__file__)] + sys.argv[1:])

    camwalld = load_camwalld()
    camwalld.load_conf(CONF)
    cfg = camwalld.mqtt_settings()          # exits 78 with a message on a bad setting
    if cfg is None:
        step("settings", "MQTT=no (or unset): camwalld does not use MQTT")
        return
    step("settings", f"{cfg['host']}:{cfg['port']}, TLS {'yes' if cfg['tls'] else 'no'}"
         f"{', CA ' + cfg['ca'] if cfg['ca'] else ''}, user {cfg['user'] or '(none)'}, "
         f"password {'set' if os.environ.get('MQTT_PASSWORD') else '(none)'}")
    step("", f"topic {cfg['topic']}, discovery {cfg['discovery'] or 'off'}, "
         f"client id {cfg['client_id']}")

    try:
        addrs = sorted({a[4][0] for a in socket.getaddrinfo(cfg["host"], cfg["port"],
                                                            type=socket.SOCK_STREAM)})
    except socket.gaierror as e:
        fail("name lookup", e.strerror)
    step("name lookup", ", ".join(addrs))

    problem = camwalld.connect_problem(cfg["host"], cfg["port"], cfg["tls"], cfg["ca"])
    if problem:
        fail("TCP/TLS", problem)
    step("TCP", "connects")
    if cfg["tls"]:
        ctx = ssl.create_default_context(cafile=cfg["ca"])
        with socket.create_connection((cfg["host"], cfg["port"]), timeout=5) as sock, \
                ctx.wrap_socket(sock, server_hostname=cfg["host"]) as tls:
            cert = tls.getpeercert()
            name = lambda field: ", ".join(v for rdn in cert.get(field, ())
                                          for k, v in rdn if k in ("commonName", "organizationName"))
            expires = datetime.datetime.fromtimestamp(ssl.cert_time_to_seconds(cert["notAfter"]))
            step("TLS", f"{tls.version()}, certificate valid for this name")
            step("", f"subject {name('subject')}; issuer {name('issuer')}; "
                 f"expires {expires:%Y-%m-%d}")

    import paho.mqtt.client as mqtt
    result, messages = {}, []
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                    client_id=f"{cfg['client_id']}-test", protocol=mqtt.MQTTv311)
    if cfg["user"]:
        c.username_pw_set(cfg["user"], os.environ.get("MQTT_PASSWORD") or None)
    if cfg["tls"]:
        c.tls_set(ca_certs=cfg["ca"])
    c.on_connect = lambda cl, u, f, reason, p: result.setdefault("reason", reason)
    c.on_message = lambda cl, u, m: messages.append(m)
    try:
        c.connect(cfg["host"], cfg["port"], keepalive=30)
    except (OSError, ssl.SSLError) as e:
        fail("MQTT", str(e))
    c.loop_start()
    for _ in range(50):
        if "reason" in result:
            break
        time.sleep(0.1)
    reason = result.get("reason")
    if reason is None:
        c.loop_stop()
        fail("MQTT", "no answer to the login within 5 s")
    if reason.is_failure:
        c.loop_stop()
        fail("MQTT", f"login refused: {reason}")
    step("MQTT", "logged in")

    t = cfg["topic"]
    c.subscribe([(f"{t}/#", 1)] + ([(f"{cfg['discovery']}/+/{t.replace('/', '_')}/+/config", 1)]
                                   if cfg["discovery"] else []))
    if args.set:
        c.publish(f"{t}/mode/set", args.set, qos=1).wait_for_publish(5)
        step("published", f"{t}/mode/set = {args.set}")
    time.sleep(args.wait)
    c.disconnect()
    c.loop_stop()

    if not messages:
        step("topics", f"nothing under {t}/ yet (camwalld not connected, or MQTT_TOPIC differs)")
    for m in messages:
        payload = m.payload.decode(errors="replace")
        if m.topic.endswith("/config"):
            payload = "(Home Assistant discovery config)"
        step("topics" if m is messages[0] else "", f"{m.topic} = {payload}"
             f"{'' if m.retain else '  (live)'}")


if __name__ == "__main__":
    main()
