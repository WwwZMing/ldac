"""Parse a netsh/BTHPORT BIP trace (bt-pairing.xml) into human-readable
L2CAP + AVDTP conversations.

Output: a chronological transcript of the A2DP session (what the healthy
Windows stack actually puts on the wire), used as the reference for our
own kernel implementation.
"""
import re
import sys
import collections

XML = sys.argv[1] if len(sys.argv) > 1 else r'C:\Developer\ldac-driver\log\bt-pairing.xml'

AVDTP_SIG = {
    0x01: 'Discover', 0x02: 'GetCapabilities', 0x03: 'SetConfiguration',
    0x04: 'GetConfiguration', 0x05: 'Reconfigure', 0x06: 'Open',
    0x07: 'Start', 0x08: 'Close', 0x09: 'Suspend', 0x0A: 'Abort',
    0x0B: 'SecurityControl', 0x0C: 'GetAllCapabilities', 0x0D: 'DelayReport',
}
L2CAP_SIG = {
    0x01: 'CommandReject', 0x02: 'ConnectionRequest', 0x03: 'ConnectionResponse',
    0x04: 'ConfigRequest', 0x05: 'ConfigResponse', 0x06: 'DisconnectionRequest',
    0x07: 'DisconnectionResponse', 0x08: 'EchoRequest', 0x09: 'EchoResponse',
    0x0A: 'InfoRequest', 0x0B: 'InfoResponse',
}
AVDTP_MT = {0: 'CMD', 1: 'GEN-REJ', 2: 'RSP', 3: 'RSP-REJ'}


def load_rows(path):
    txt = open(path, encoding='utf-8', errors='ignore').read()
    evs = re.findall(r'<Event[^>]*>.*?</Event>', txt, re.S)
    rows = []
    for ev in evs:
        t = re.search(r'BIP_Type">([^<]*)<', ev)
        d = re.search(r'BIP_Data">(0x[0-9a-fA-F]+)<', ev)
        tm = re.search(r'SystemTime="([^"]+)"', ev)
        if t and d:
            rows.append((tm.group(1) if tm else '',
                         int(t.group(1)),
                         bytes.fromhex(d.group(1)[2:])))
    rows.sort(key=lambda r: r[0])
    return rows


def parse_acl(pkt):
    """HCI ACL packet body -> (handle, pb, payload) or None."""
    if len(pkt) < 4:
        return None
    h = pkt[0] | (pkt[1] << 8)
    handle = h & 0x0FFF
    pb = (h >> 12) & 0x3
    dlen = pkt[2] | (pkt[3] << 8)
    return handle, pb, pkt[4:4 + dlen]


def main():
    rows = load_rows(XML)
    print(f'total BIP rows: {len(rows)}')

    # CID -> stats, to find the AVDTP signaling channel
    cid_stats = collections.defaultdict(lambda: {'tx': 0, 'rx': 0, 'len': 0})
    frames = []            # (time, dir, cid, l2payload)
    for tm, typ, pkt in rows:
        if typ not in (3, 4):          # 3 = TX, 4 = RX
            continue
        d = parse_acl(pkt)
        if not d:
            continue
        handle, pb, payload = d
        if pb != 2 or len(payload) < 4:   # only first fragments carry L2CAP hdr
            continue
        l2len = payload[0] | (payload[1] << 8)
        cid = payload[2] | (payload[3] << 8)
        data = payload[4:4 + l2len]
        dname = 'TX' if typ == 3 else 'RX'
        cid_stats[cid]['tx' if typ == 3 else 'rx'] += 1
        cid_stats[cid]['len'] += len(data)
        frames.append((tm, dname, cid, data))

    print('\n=== L2CAP channels seen ===')
    for cid in sorted(cid_stats):
        s = cid_stats[cid]
        print(f'  CID 0x{cid:04X}: tx={s["tx"]:5d} rx={s["rx"]:5d} bytes={s["len"]:7d}')

    # AVDTP signaling rides a dynamic CID; identify it by payload shape
    avdtp_cids = set()
    for tm, dname, cid, data in frames:
        if cid == 1 or cid < 0x40 or len(data) < 2:
            continue
        sig = data[1] & 0x3F
        mt = data[0] & 0x03
        pt = (data[0] >> 2) & 0x03
        if sig in AVDTP_SIG and mt in (0, 2) and pt == 0:
            avdtp_cids.add(cid)

    print(f'\n=== AVDTP signaling CIDs: {[hex(c) for c in sorted(avdtp_cids)]} ===\n')

    for tm, dname, cid, data in frames:
        if cid not in avdtp_cids or len(data) < 2:
            continue
        tl = data[0] >> 4
        pt = (data[0] >> 2) & 0x03
        mt = data[0] & 0x03
        sig = data[1] & 0x3F
        body = data[2:].hex(' ')
        name = AVDTP_SIG.get(sig, f'0x{sig:02X}')
        print(f'{tm[11:23]} {dname} cid=0x{cid:04X} TL={tl} {AVDTP_MT.get(mt)} {name:<18} len={len(data)-2:3d} {body}')

    # also dump L2CAP signaling (channel setup is interesting for PSM 25)
    print('\n=== L2CAP signaling (CID 0x0001) ===')
    for tm, dname, cid, data in frames:
        if cid != 1 or len(data) < 4:
            continue
        code = data[0]
        ident = data[1]
        ln = data[2] | (data[3] << 8)
        body = data[4:4 + ln]
        nm = L2CAP_SIG.get(code, f'0x{code:02X}')
        print(f'{tm[11:23]} {dname} {nm:<20} id={ident} len={ln} {body.hex(" ")}')


if __name__ == '__main__':
    main()
