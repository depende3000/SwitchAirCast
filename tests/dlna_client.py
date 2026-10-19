#!/usr/bin/env python3
"""A UPnP control point that exercises the DLNA renderer the way BubbleUPnP or Windows
"Cast to Device" do: SSDP discovery, descriptions, AVTransport / RenderingControl /
ConnectionManager actions and GENA events.

    tests/dlna_client.py build/linux/dlna_test     starts the simulated renderer and tests it
    tests/dlna_client.py --url http://IP:PORT/description.xml
                                                   tests a running receiver (only the safe parts)
"""

import argparse
import http.server
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

SSDP = ("239.255.255.250", 1900)
DEV = "{urn:schemas-upnp-org:device-1-0}"
AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
CM = "urn:schemas-upnp-org:service:ConnectionManager:1"

failures = 0


def check(cond, what):
    global failures
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures += 1


def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


# ---- the simulated renderer's stdout ----

class Harness:
    def __init__(self, path):
        self.proc = subprocess.Popen([path, "120"], stdout=subprocess.PIPE, text=True)
        self.lines = []
        self.cv = threading.Condition()
        self.port = None
        threading.Thread(target=self._read, daemon=True).start()
        self.wait(lambda l: l.startswith("PORT "), 5)
        self.port = int(next(l for l in self.lines if l.startswith("PORT ")).split()[1])

    def _read(self):
        for line in self.proc.stdout:
            with self.cv:
                self.lines.append(line.rstrip("\n"))
                self.cv.notify_all()

    def wait(self, pred, timeout):
        end = time.time() + timeout
        with self.cv:
            while True:
                for l in self.lines:
                    if pred(l):
                        return l
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def mark(self):
        with self.cv:
            n = len(self.lines)
        return n

    def wait_after(self, mark, pred, timeout):
        end = time.time() + timeout
        with self.cv:
            while True:
                for l in self.lines[mark:]:
                    if pred(l):
                        return l
                left = end - time.time()
                if left <= 0:
                    return None
                self.cv.wait(left)

    def close(self):
        self.proc.terminate()
        self.proc.wait(5)


# ---- event receiver and a tiny media/art server ----

events = []
events_cv = threading.Condition()
ART = b"\x89PNG\r\n\x1a\n" + b"x" * 500


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_NOTIFY(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode()
        with events_cv:
            events.append((self.path, self.headers.get("SID"), int(self.headers.get("SEQ", "-1")), body))
            events_cv.notify_all()
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "image/png")
        self.send_header("Content-Length", str(len(ART)))
        self.end_headers()
        self.wfile.write(ART)


def last_change_states(path):
    with events_cv:
        out = []
        for p, _, _, body in events:
            if p != path:
                continue
            lc = ET.fromstring(body).find(".//LastChange")
            if lc is None or not lc.text:
                continue
            ev = ET.fromstring(lc.text)
            for el in ev.iter():
                if el.tag.endswith("TransportState"):
                    out.append(el.get("val"))
        return out


def wait_state(path, state, timeout):
    end = time.time() + timeout
    with events_cv:
        while state not in last_change_states_unlocked(path):
            left = end - time.time()
            if left <= 0:
                return False
            events_cv.wait(left)
    return True


def last_change_states_unlocked(path):
    out = []
    for p, _, _, body in events:
        if p != path:
            continue
        lc = ET.fromstring(body).find(".//LastChange")
        if lc is None or not lc.text:
            continue
        for el in ET.fromstring(lc.text).iter():
            if el.tag.endswith("TransportState"):
                out.append(el.get("val"))
    return out


def clear_events():
    with events_cv:
        events.clear()


# ---- UPnP helpers ----

def soap(control, service, action, args=(), expect_error=None):
    body = "".join("<%s>%s</%s>" % (k, escape(str(v)), k) for k, v in args)
    env = ('<?xml version="1.0" encoding="utf-8"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:%s xmlns:u="%s">%s</u:%s>'
           '</s:Body></s:Envelope>' % (action, service, body, action))
    req = urllib.request.Request(control, data=env.encode(), method="POST", headers={
        "Content-Type": 'text/xml; charset="utf-8"',
        "SOAPACTION": '"%s#%s"' % (service, action),
        "User-Agent": "BubbleUPnP/4.2.1 (Android 14)",
    })
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            root = ET.fromstring(r.read())
            resp = root.find(".//{%s}%sResponse" % (service, action))
            return {c.tag: (c.text or "") for c in resp} if resp is not None else {}
    except urllib.error.HTTPError as e:
        root = ET.fromstring(e.read())
        code = root.find(".//{urn:schemas-upnp-org:control-1-0}errorCode")
        code = int(code.text) if code is not None else e.code
        if expect_error is None:
            raise RuntimeError("%s failed with UPnP error %s" % (action, code))
        return code


def gena(method, url, headers):
    req = urllib.request.Request(url, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers)


def ssdp_search(st, timeout=2.5):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    s.settimeout(0.3)
    msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n" % st)
    found = []
    end = time.time() + timeout
    s.sendto(msg.encode(), SSDP)
    while time.time() < end:
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        hdr = dict((k.strip().upper(), v.strip()) for k, v in
                   (l.split(":", 1) for l in data.decode(errors="replace").split("\r\n")[1:] if ":" in l))
        found.append(hdr)
    s.close()
    return found


def listen_notify(timeout):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    s.bind(("", 1900))
    mreq = socket.inet_aton(SSDP[0]) + socket.inet_aton("0.0.0.0")
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    s.settimeout(0.5)
    got = []
    end = time.time() + timeout
    while time.time() < end:
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        t = data.decode(errors="replace")
        if t.startswith("NOTIFY") and "ssdp:alive" in t and "SwitchAirCast" in t:
            got.append(t)
    s.close()
    return got


def describe(location):
    with urllib.request.urlopen(location, timeout=5) as r:
        root = ET.fromstring(r.read())
    dev = root.find(DEV + "device")
    base = re.match(r"(http://[^/]+)", location).group(1)
    svcs = {}
    for s in dev.iter(DEV + "service"):
        svcs[s.find(DEV + "serviceType").text] = {
            "scpd": base + s.find(DEV + "SCPDURL").text,
            "control": base + s.find(DEV + "controlURL").text,
            "event": base + s.find(DEV + "eventSubURL").text,
        }
    return dev, svcs


def didl(title, artist, album, art, url):
    item = ('<DIDL-Lite xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/" xmlns:dc="http://purl.org/dc/elements/1.1/" '
            'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/"><item id="1" parentID="0" restricted="1">'
            '<dc:title>%s</dc:title><upnp:artist>%s</upnp:artist><upnp:album>%s</upnp:album>'
            '<upnp:albumArtURI>%s</upnp:albumArtURI><upnp:class>object.item.audioItem.musicTrack</upnp:class>'
            '<res protocolInfo="http-get:*:audio/mpeg:*">%s</res></item></DIDL-Lite>'
            % (escape(title), escape(artist), escape(album), escape(art), escape(url)))
    return item


def transport(avt):
    return soap(avt, AVT, "GetTransportInfo", [("InstanceID", 0)])["CurrentTransportState"]


def position(avt):
    t = soap(avt, AVT, "GetPositionInfo", [("InstanceID", 0)])["RelTime"]
    h, m, s = (int(x) for x in t.split(":"))
    return h * 3600 + m * 60 + s


def wait_transport(avt, state, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if transport(avt) == state:
            return True
        time.sleep(0.25)
    return False


def play_on_receiver(avt, media):
    """Plays a real file on a running receiver and checks that it plays, pauses, seeks and stops."""
    print("Playing %s" % media)
    I = [("InstanceID", 0)]
    soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", media), ("CurrentURIMetaData", "")])
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    check(wait_transport(avt, "PLAYING", 20), "PLAYING")
    p0 = position(avt)
    time.sleep(3)
    p1 = position(avt)
    check(p1 >= p0 + 2, "position advances (%d -> %d s)" % (p0, p1))
    dur = soap(avt, AVT, "GetPositionInfo", I)["TrackDuration"]
    check(dur != "0:00:00", "duration known (%s)" % dur)
    soap(avt, AVT, "Pause", I)
    check(wait_transport(avt, "PAUSED_PLAYBACK", 3), "PAUSED_PLAYBACK")
    soap(avt, AVT, "Seek", I + [("Unit", "REL_TIME"), ("Target", "0:00:10")])
    time.sleep(1)
    check(abs(position(avt) - 10) <= 1, "seek to 0:00:10 while paused (%d s)" % position(avt))
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    check(wait_transport(avt, "PLAYING", 3), "PLAYING again")
    soap(avt, AVT, "Stop", I)
    check(wait_transport(avt, "STOPPED", 5), "STOPPED")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("harness", nargs="?")
    ap.add_argument("--url")
    ap.add_argument("--play", help="with --url: a media URL for the receiver to play")
    a = ap.parse_args()

    harness = Harness(a.harness) if a.harness else None
    me = local_ip()
    srv = http.server.ThreadingHTTPServer(("", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    cb_base = "http://%s:%d" % (me, srv.server_address[1])

    print("SSDP")
    notifies = listen_notify(4) if harness else []
    if harness:
        check(len(notifies) > 0, "renderer announces itself (NOTIFY ssdp:alive)")
    res = ssdp_search("urn:schemas-upnp-org:device:MediaRenderer:1")
    ours = [r for r in res if "SwitchAirCast" in r.get("SERVER", "")]
    check(len(ours) > 0, "answers an M-SEARCH for MediaRenderer:1")
    location = a.url or (ours[0]["LOCATION"] if ours else "http://127.0.0.1:%d/description.xml" % harness.port)
    if ours:
        check(ours[0]["USN"].endswith("::urn:schemas-upnp-org:device:MediaRenderer:1"), "USN names the device type")
    allres = [r for r in ssdp_search("ssdp:all") if "SwitchAirCast" in r.get("SERVER", "")]
    check(len({r["ST"] for r in allres}) >= 6, "ssdp:all lists the root device, UUID, device and 3 services")

    print("Descriptions")
    dev, svcs = describe(location)
    check(dev.find(DEV + "deviceType").text == "urn:schemas-upnp-org:device:MediaRenderer:1", "device type")
    check(dev.find(DEV + "UDN").text.startswith("uuid:"), "UDN")
    check(set(svcs) == {AVT, RCS, CM}, "three services")
    for t, s in svcs.items():
        with urllib.request.urlopen(s["scpd"], timeout=5) as r:
            scpd = ET.fromstring(r.read())
        n = len(scpd.findall(".//{urn:schemas-upnp-org:service-1-0}action"))
        check(n > 0, "%s SCPD parses (%d actions)" % (t.split(":")[3], n))

    avt, rcs, cm = svcs[AVT]["control"], svcs[RCS]["control"], svcs[CM]["control"]
    sink = soap(cm, CM, "GetProtocolInfo")["Sink"]
    check("video/mp4" in sink and "audio/mpeg" in sink, "sink protocols include MP4 and MP3")
    check(soap(cm, CM, "GetCurrentConnectionIDs")["ConnectionIDs"] == "0", "connection IDs")

    if not harness:
        I = [("InstanceID", 0)]
        info = soap(avt, AVT, "GetTransportInfo", I)
        print("  transport state:", info["CurrentTransportState"])
        print("  volume:", soap(rcs, RCS, "GetVolume", I + [("Channel", "Master")])["CurrentVolume"])
        if a.play:
            play_on_receiver(avt, a.play)
        print("%d failure(s)" % failures)
        return 1 if failures else 0

    print("Events")
    status, h = gena("SUBSCRIBE", svcs[AVT]["event"], {"CALLBACK": "<%s/avt>" % cb_base, "NT": "upnp:event",
                                                         "TIMEOUT": "Second-300"})
    check(status == 200 and h.get("SID", "").startswith("uuid:"), "SUBSCRIBE AVTransport")
    sid = h.get("SID")
    check(h.get("TIMEOUT") == "Second-300", "subscription timeout echoed")
    status, h2 = gena("SUBSCRIBE", svcs[RCS]["event"], {"CALLBACK": "<%s/rcs>" % cb_base, "NT": "upnp:event"})
    check(status == 200, "SUBSCRIBE RenderingControl")
    check(wait_state("/avt", "NO_MEDIA_PRESENT", 3), "initial event says NO_MEDIA_PRESENT")
    with events_cv:
        seqs = [e[2] for e in events if e[0] == "/avt"]
    check(seqs[:1] == [0], "initial event has SEQ 0")
    status, _ = gena("SUBSCRIBE", svcs[AVT]["event"], {"SID": sid, "TIMEOUT": "Second-300"})
    check(status == 200, "renewal")
    status, _ = gena("SUBSCRIBE", svcs[AVT]["event"], {"SID": "uuid:nope"})
    check(status == 412, "renewing an unknown SID gives 412")

    print("Playback")
    I = [("InstanceID", 0)]
    check(soap(avt, AVT, "Pause", I, expect_error=True) == 701, "Pause with no media gives 701")
    check(soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", "rtsp://x/y"), ("CurrentURIMetaData", "")],
               expect_error=True) == 714, "a non-HTTP URI gives 714")
    check(soap(avt, AVT, "GetTransportInfo", [("InstanceID", 1)], expect_error=True) == 718, "InstanceID 1 gives 718")

    url1 = "http://%s/song1.mp3?a=1&b=2" % me
    meta = didl("Café & Crème <live>", "Ármin", "Álbum", cb_base + "/art.png", url1)
    m = harness.mark()
    soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", url1), ("CurrentURIMetaData", meta)])
    info = soap(avt, AVT, "GetMediaInfo", I)
    check(info["CurrentURI"] == url1, "GetMediaInfo returns the URI with & intact")
    check(info["CurrentURIMetaData"] == meta, "metadata is returned unchanged")
    check(soap(avt, AVT, "GetTransportInfo", I)["CurrentTransportState"] == "STOPPED", "STOPPED after SetAVTransportURI")
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    line = harness.wait_after(m, lambda l: l.startswith("EVENT play"), 3)
    check(line is not None and "title=Café & Crème <live>" in line and "artist=Ármin" in line, "title and artist decoded")
    check(line is not None and "client=BubbleUPnP" in line, "client name from the User-Agent")
    check(harness.wait_after(m, lambda l: l.startswith("EVENT coverart 508"), 3) is not None, "cover art fetched")
    check(wait_state("/avt", "TRANSITIONING", 3), "event TRANSITIONING")
    check(wait_state("/avt", "PLAYING", 3), "event PLAYING")
    pos = soap(avt, AVT, "GetPositionInfo", I)
    check(pos["TrackDuration"] == "0:00:03" and pos["TrackURI"] == url1, "GetPositionInfo")
    check(soap(avt, AVT, "GetCurrentTransportActions", I)["Actions"].startswith("Pause"), "actions while playing")

    soap(avt, AVT, "Pause", I)
    check(wait_state("/avt", "PAUSED_PLAYBACK", 3), "event PAUSED_PLAYBACK")
    m = harness.mark()
    soap(avt, AVT, "Seek", I + [("Unit", "REL_TIME"), ("Target", "0:00:01.500")])
    check(harness.wait_after(m, lambda l: l == "EVENT seek 1.5", 2) is not None, "Seek REL_TIME 0:00:01.500")
    check(soap(avt, AVT, "Seek", I + [("Unit", "FRAME"), ("Target", "1")], expect_error=True) == 710,
          "unknown seek unit gives 710")
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    check(harness.wait_after(m, lambda l: l == "EVENT paused=0", 2) is not None, "Play resumes")

    url2 = "http://%s/song2.mp3" % me
    soap(avt, AVT, "SetNextAVTransportURI", I + [("NextURI", url2), ("NextURIMetaData", "")])
    check("Next" in soap(avt, AVT, "GetCurrentTransportActions", I)["Actions"], "Next offered once queued")
    m = harness.mark()
    check(harness.wait_after(m, lambda l: l.startswith("EVENT play url=" + url2), 6) is not None,
          "the queued track starts at the end of the current one")
    time.sleep(0.5)
    info = soap(avt, AVT, "GetMediaInfo", I)
    check(info["CurrentURI"] == url2 and info["NextURI"] == "", "the queued track became the current one")

    clear_events()
    soap(avt, AVT, "Stop", I)
    check(wait_state("/avt", "STOPPED", 3), "event STOPPED after Stop")

    m = harness.mark()
    soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", "http://%s/long.mp4" % me), ("CurrentURIMetaData", "")])
    soap(avt, AVT, "Seek", I + [("Unit", "REL_TIME"), ("Target", "0:01:00")])
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    check(harness.wait_after(m, lambda l: "long.mp4 start=60.0" in l, 2) is not None, "Seek while stopped sets the start")
    time.sleep(0.6)
    m = harness.mark()
    soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", "http://%s/other.mp4" % me), ("CurrentURIMetaData", "")])
    check(harness.wait_after(m, lambda l: "other.mp4 start=0.0" in l, 2) is not None,
          "a new URI while playing starts right away")
    soap(avt, AVT, "Stop", I)

    soap(avt, AVT, "SetAVTransportURI", I + [("CurrentURI", "http://%s/fail.mp4" % me), ("CurrentURIMetaData", "")])
    soap(avt, AVT, "Play", I + [("Speed", "1")])
    time.sleep(0.8)
    check(soap(avt, AVT, "GetTransportInfo", I)["CurrentTransportStatus"] == "ERROR_OCCURRED",
          "a failed media reports ERROR_OCCURRED")

    print("Volume")
    C = I + [("Channel", "Master")]
    m = harness.mark()
    soap(rcs, RCS, "SetVolume", C + [("DesiredVolume", 40)])
    check(soap(rcs, RCS, "GetVolume", C)["CurrentVolume"] == "40", "SetVolume / GetVolume")
    soap(rcs, RCS, "SetMute", C + [("DesiredMute", 1)])
    check(harness.wait_after(m, lambda l: l == "EVENT volume 0", 2) is not None, "mute silences")
    check(soap(rcs, RCS, "GetMute", C)["CurrentMute"] == "1", "GetMute")
    check(soap(rcs, RCS, "GetVolume", C)["CurrentVolume"] == "40", "volume kept while muted")
    soap(rcs, RCS, "SetMute", C + [("DesiredMute", 0)])
    check(harness.wait_after(m, lambda l: l == "EVENT volume 40", 2) is not None, "unmute restores the volume")
    check(soap(rcs, RCS, "SetVolume", C + [("DesiredVolume", 101)], expect_error=True) == 402, "volume 101 gives 402")
    time.sleep(0.6)
    with events_cv:
        rcs_events = [e[3] for e in events if e[0] == "/rcs"]
    check(any("Volume channel=&quot;Master&quot; val=&quot;40&quot;" in b for b in rcs_events), "volume event")

    status, _ = gena("UNSUBSCRIBE", svcs[AVT]["event"], {"SID": sid})
    check(status == 200, "UNSUBSCRIBE")

    harness.close()
    print("%d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
