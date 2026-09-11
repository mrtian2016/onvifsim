# Security Policy

## What onvifsim is

A test tool. It simulates ONVIF cameras so you can exercise client software against
device behavior you do not have on your desk. It is meant to run on a lab network or on
your own machine, alongside the client you are testing.

It is **not** a production service, and a few things in it are deliberately insecure
because making them secure would defeat the purpose:

| Thing | Why it is that way |
|---|---|
| Device credentials default to `admin` / `admin123` | So the quick-start commands in the README actually work. Change them in the GUI or the scenario file. |
| The TP-Link VIGI persona ships a self-signed certificate **with its private key in this repository** | Qt has no API for generating a certificate at runtime, and this project takes no third-party dependencies. The key exists so quirk `transport.self_signed_tls` can offer a TLS port that clients must `verify=False` to reach. It protects nothing. |
| Several quirks make the simulator behave badly on purpose | Reproducing broken firmware is the point of the project. |

None of the above is a vulnerability. Please do not report them as such.

## What is a vulnerability

Anything that lets someone who can reach onvifsim do more than "talk to a fake camera":

- Executing commands, reading files, or writing files outside what the feature intends —
  in particular anything reachable from the REST control API or from a crafted ONVIF,
  RTSP or vendor-API request.
- Escaping into the privileged helper used for IP aliases (`pkexec` / `osascript` /
  `netsh`). That path takes an interface name from user input; it is validated and every
  argument is quoted, but it is the highest-value target in the codebase.
- Crashes that are remotely triggerable and look like memory corruption rather than a
  missing null check.
- A cross-origin web page driving the control API. The control API sends no CORS headers
  unless a token is configured, precisely to prevent this.

## How to report

Open a [private security advisory](https://github.com/mrtian2016/onvifsim/security/advisories/new)
on GitHub. Please do not open a public issue for a vulnerability first.

Include what you sent, what happened, and the version (`onvifsim --version`). A reproducer
is worth more than a description. There is no bounty; this is a hobby-scale project.

## Hardening notes for users

- The control API binds to `127.0.0.1` and has **no authentication** unless you pass
  `--control-token`. If you move it off loopback — `--control-bind 0.0.0.0`, containers —
  set a token. onvifsim logs a warning when you do not.
- Independent-IP mode adds real IP aliases to a real interface and needs elevation. If you
  do not want that, use the default port mode; nothing is lost but the address layout.
- Do not expose an onvifsim instance to an untrusted network. It is a simulator of
  insecure devices; it is not hardened, and it is not trying to be.
