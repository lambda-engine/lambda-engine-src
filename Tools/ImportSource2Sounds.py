#!/usr/bin/env python3
"""
Imports Half-Life: Alyx sound events into a Source 1 style soundscript plus .wav files.

Source 2 keeps sounds as .vsnd_c (compiled) and maps names to files in .vsndevts_c soundevent scripts; Source 1
(what Lambda Engine runs) uses scripts/game_sounds_*.txt KeyValues and plain .wav files. This tool reads one HL:A
soundevent script with ValveResourceFormat's CLI, decodes the referenced .vsnd_c to PCM .wav (16-bit, downmixed to
mono - Source 1 only spatialises mono), and writes a soundscript whose entries carry the soundevent's volume and
random pitch. An explicit name map lets HL2 script names (what the ported game code emits) pick up HL:A events.

Usage:
  python ImportSource2Sounds.py --vrf Source2Viewer-CLI.dll --pak "...\\hlvr\\pak01_dir.vpk" --out <moddir>
      --events soundevents/soundevents_creature_hc_classic.vsndevts_c
      --script scripts/npc_sounds_headcrab_hla.txt --prefix hla
      --map "NPC_HeadCrab.Idle=NPC_HeadCrab.Idle_Anim" --map "NPC_HeadCrab.Alert=NPC_Headcrab.Alerted" ...

The converted waves land in <moddir>/sound/<prefix>/<original path>.wav; put the generated script before the stock
one in scripts/game_sounds_manifest.txt - the first definition of a name wins, as in CSoundEmitterSystemBase.
Only events with vsnd files are written; "hlvr_start_multi" events (several events played at once) are flattened to
their first child unless mapped to a specific one.

HL:A's explosions are layered: an "hlvr_explosions" event carries vsnd_files_mid, _mech, _lfe and _distant lists
(the body, the transient, the bass, and what plays far away) that the game mixes by distance. Source 1 plays one
wave, so --mix-layers mid,mech,lfe sums the named layers, take for take, into one wave per take.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import wave


def run_vrf(vrf, args):
    cmd = ['dotnet', vrf] + args
    result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-2000:] + result.stderr[-2000:])
        raise RuntimeError('VRF failed: ' + ' '.join(args))
    return result.stdout


def parse_kv3_events(text):
    """
    A deliberately small reader for the KV3 text VRF prints for a .vsndevts_c: top-level "Name = { key = value ...
    vsnd_files = [ "..." ] }" blocks. Returns {name: {key: value, 'vsnd_files': [...]}}.
    """
    events = {}
    block = re.compile(r'^\t([A-Za-z0-9_.]+)\s*=\s*\n\t\{(.*?)^\t\}', re.S | re.M)
    for m in block.finditer(text):
        name, body = m.group(1), m.group(2)
        ev = {}
        ev['vsnd_files'] = []
        for list_name, items in re.findall(r'(vsnd_files(?:_[a-z]+)?)\s*=\s*\[(.*?)\]', body, re.S):
            ev[list_name] = re.findall(r'"([^"]+)"', items)
        for k, v in re.findall(r'^\t\t([A-Za-z0-9_]+)\s*=\s*([^\n\[]+?)\s*$', body, re.M):
            if k.startswith('vsnd_files'):
                continue
            ev[k] = v.strip().strip('"')
        events[name] = ev
    return events


def resolve_files(events, name, layers=None, depth=0):
    """
    The vsnd list of an event, following hlvr_start_multi / soundevent_01 chains to the first child with files.
    With layers (e.g. ['mid', 'mech', 'lfe']) a layered event yields one tuple per take, a file from each layer
    that has one (shorter layers repeat), for the caller to mix.
    """
    ev = events.get(name)
    if not ev or depth > 4:
        return None, []
    if ev['vsnd_files']:
        return ev, ev['vsnd_files']
    if layers:
        lists = [ev.get('vsnd_files_' + layer, []) for layer in layers]
        lists = [l for l in lists if l]
        if lists:
            takes = max(len(l) for l in lists)
            return ev, [tuple(l[i % len(l)] for l in lists) for i in range(takes)]
    for key in sorted(k for k in ev if re.match(r'soundevent(_\d+)?$', k)):
        child, files = resolve_files(events, ev[key], layers, depth + 1)
        if files:
            return child, files
    return ev, []


def read_mono16(src):
    """A PCM wav as (rate, 16-bit mono samples) - Source 1 spatialises mono only."""
    with wave.open(src, 'rb') as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        fmt = '<%dh' % (len(raw) // 2)
        samples = struct.unpack(fmt, raw)
    elif sw == 1:
        samples = [(b - 128) << 8 for b in raw]
    elif sw == 3:
        samples = [int.from_bytes(raw[i:i + 3], 'little', signed=True) >> 8 for i in range(0, len(raw), 3)]
    elif sw == 4:
        samples = [s >> 16 for s in struct.unpack('<%di' % (len(raw) // 4), raw)]
    else:
        raise RuntimeError(f'{src}: unsupported sample width {sw}')
    if ch > 1:
        samples = [max(-32768, min(32767, sum(samples[i:i + ch]) // ch)) for i in range(0, len(samples), ch)]
    return rate, samples


def write_mono16(dst, rate, samples):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with wave.open(dst, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))
    return rate, len(samples) / float(rate)


def to_mono16(src, dst):
    """Rewrites a PCM wav as 16-bit mono at its own sample rate."""
    rate, samples = read_mono16(src)
    return write_mono16(dst, rate, samples)


def mix_mono16(srcs, dst):
    """
    Sums several wavs into one mono 16-bit wav. The layers of an HL:A explosion are mixed at unity, the way the
    game plays them up close; the sum is only scaled down when it would clip.
    """
    rate = None
    mixed = []
    for src in srcs:
        r, samples = read_mono16(src)
        if rate is None:
            rate = r
        elif r != rate:
            # A rare mismatch: resample by nearest neighbour rather than fail the whole sound.
            samples = [samples[min(len(samples) - 1, int(i * r / rate))] for i in range(int(len(samples) * rate / r))]
        if len(samples) > len(mixed):
            mixed.extend([0] * (len(samples) - len(mixed)))
        for i, s in enumerate(samples):
            mixed[i] += s
    peak = max(1, max(abs(s) for s in mixed))
    if peak > 32767:
        scale = 32767.0 / peak
        mixed = [int(s * scale) for s in mixed]
    return write_mono16(dst, rate, mixed)


def pitch_range(ev):
    """pitch_rand_min/max are fractions of normal pitch; Source 1 scripts use percent (PITCH_NORM = 100)."""
    lo = float(ev.get('pitch_rand_min', 0.0))
    hi = float(ev.get('pitch_rand_max', 0.0))
    if lo == 0.0 and hi == 0.0:
        return 'PITCH_NORM'
    return f'{round(100 * (1 + lo)):d}, {round(100 * (1 + hi)):d}'


def volume_value(ev):
    vol = float(ev.get('volume', 1.0))
    lo = float(ev.get('volume_rand_min', 0.0))
    hi = float(ev.get('volume_rand_max', 0.0))
    if lo or hi:
        return f'{max(0.0, vol + lo):.2f}, {min(1.0, vol + hi):.2f}'
    return 'VOL_NORM' if vol == 1.0 else f'{min(1.0, vol):.2f}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--vrf', required=True, help='path to Source2Viewer-CLI.dll')
    ap.add_argument('--pak', required=True, help="HL:A game/hlvr/pak01_dir.vpk")
    ap.add_argument('--out', required=True, help='mod directory (the one with gameinfo.txt)')
    ap.add_argument('--events', required=True, help='soundevent script inside the VPK, e.g. soundevents/soundevents_creature_hc_classic.vsndevts_c')
    ap.add_argument('--script', required=True, help='soundscript to write, relative to the mod dir (scripts/npc_sounds_headcrab_hla.txt)')
    ap.add_argument('--prefix', default='hla', help='directory under sound/ for the converted waves')
    ap.add_argument('--map', action='append', default=[], help='"ScriptName=HLA.Event": write the HL:A event under a Source 1 name')
    ap.add_argument('--channel', default='CHAN_VOICE', help='channel for every entry (NPC vocals are CHAN_VOICE)')
    ap.add_argument('--soundlevel', default='SNDLVL_IDLE', help='soundlevel for every entry (HL:A falloffs do not map 1:1)')
    ap.add_argument('--only-mapped', action='store_true', help='write only the --map entries, not every event in the file')
    ap.add_argument('--mix-layers', default='', help='"mid,mech,lfe": mix these layers of a layered (hlvr_explosions) event into one wave per take')
    args = ap.parse_args()

    text = run_vrf(args.vrf, ['-i', args.pak, '--vpk_filepath', args.events, '-b', 'DATA'])
    events = parse_kv3_events(text)
    if not events:
        raise SystemExit(f'no events parsed from {args.events}')
    print(f'[import] {len(events)} sound events in {args.events}')

    wanted = []  # (script name, event name)
    for m in args.map:
        script_name, _, event_name = m.partition('=')
        if not event_name:
            event_name = script_name
        wanted.append((script_name, event_name))
    if not args.only_mapped:
        mapped_events = {e for _, e in wanted}
        for name in events:
            if name not in mapped_events and name not in {s for s, _ in wanted}:
                wanted.append((name, name))

    tmp = tempfile.mkdtemp(prefix='hla_vsnd_')
    decoded = set()
    lines = ['// Generated by Tools/ImportSource2Sounds.py from Half-Life: Alyx %s.' % args.events,
             '// Waves are the HL:A .vsnd decoded to 16-bit mono PCM; volume and pitch come from the soundevent.',
             '// Listed before the stock script in game_sounds_manifest.txt these names win (first definition wins).',
             '']
    written = 0
    layers = [l.strip() for l in args.mix_layers.split(',') if l.strip()]

    def decode(vsnd):
        """The decoded .wav of a .vsnd in the temp dir, decoding it on first use; None when VRF has nothing."""
        src_wav = os.path.join(tmp, vsnd.replace('.vsnd', '.wav'))
        if not os.path.exists(src_wav):
            run_vrf(args.vrf, ['-i', args.pak, '-f', vsnd + '_c', '-d', '-o', tmp])
        if not os.path.exists(src_wav):
            print(f'  missing {vsnd}')
            return None
        return src_wav

    def relative_name(vsnd):
        rel = vsnd[:-len('.vsnd')] if vsnd.endswith('.vsnd') else vsnd
        return rel[len('sounds/'):] if rel.startswith('sounds/') else rel

    for script_name, event_name in wanted:
        ev, files = resolve_files(events, event_name, layers)
        if not files:
            print(f'  skip {script_name}: {event_name} has no wave files')
            continue
        waves = []
        for take in files:
            if isinstance(take, tuple):
                # A layered take is named for what its layers' names share, plus the take number:
                # wpn_grenade_explo_digital_01 + _head_01 + _boom_01 -> wpn_grenade_explo_01.
                names = [relative_name(v) for v in take]
                number = re.search(r'_\d+$', names[0])
                prefix = os.path.commonprefix(names).rstrip('_')
                rel = prefix + number.group(0) if prefix and number else names[0]
                out_wav = os.path.join(args.out, 'sound', args.prefix, rel + '.wav')
                key = '+'.join(take)
                if key not in decoded:
                    decoded.add(key)
                    srcs = [decode(v) for v in take]
                    if any(s is None for s in srcs):
                        continue
                    mix_mono16(srcs, out_wav)
            else:
                vsnd = take
                rel = relative_name(vsnd)
                out_wav = os.path.join(args.out, 'sound', args.prefix, rel + '.wav')
                if vsnd not in decoded:
                    decoded.add(vsnd)
                    src_wav = decode(vsnd)
                    if src_wav is None:
                        continue
                    to_mono16(src_wav, out_wav)
            waves.append(f'{args.prefix}/{rel}.wav')
        if not waves:
            continue
        lines.append(f'"{script_name}"')
        lines.append('{')
        lines.append(f'\t"channel"\t\t"{args.channel}"')
        lines.append(f'\t"volume"\t\t"{volume_value(ev)}"')
        lines.append(f'\t"pitch"\t\t\t"{pitch_range(ev)}"')
        lines.append(f'\t"soundlevel"\t"{args.soundlevel}"')
        if len(waves) == 1:
            lines.append(f'\t"wave"\t\t\t"{waves[0]}"')
        else:
            lines.append('\t"rndwave"')
            lines.append('\t{')
            for w in waves:
                lines.append(f'\t\t"wave"\t"{w}"')
            lines.append('\t}')
        lines.append('}')
        lines.append('')
        written += 1
        print(f'  {script_name} <- {event_name} ({len(waves)} waves)')

    out_path = os.path.join(args.out, args.script)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print(f'[import] wrote {out_path} ({written} entries, {len(decoded)} waves)')


if __name__ == '__main__':
    main()
