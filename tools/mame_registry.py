#!/usr/bin/env python3
"""Generates src/content/machines_mame.inc from MAME's src/mame/handheld/hh_sm510.cpp.

Usage: tools/mame_registry.py /path/to/mame [--report]

Also writes docs/COMPATIBILITY.md, with the clock layouts from src/content/rtc_layouts.inc.

Machines on the presets in PRESETS are emitted, together with the board
variations LCD Lab models: input lines tied together (tnmarebc), Konami's
sample chip (ktmnt2, kst25, ktopgun2), the LED layout of elbaskb, buttons
wired to two contacts (PORT_CONDITION on a FAKE port), dials, and keypads
(IPT_KEYPAD keys go to the libretro keyboard using MAME's PORT_CODE).
Everything else is listed with the reason it was skipped. Libretro button
assignments are made here from the MAME input types.
"""
import re
import sys

PRESETS = {
    "sm5a_common": ("SM5XX_SM5A", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM5A_R1_PIEZO_MUX", "LCD_LAYOUT_SINGLE", 1),
    "sm510_common": ("SM5XX_SM510", "2", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_SINGLE", 1),
    "sm510_tiger": ("SM5XX_SM510", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_SINGLE", 1),
    "sm510_dualv": ("SM5XX_SM510", "2", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_DUAL_VERTICAL", 2),
    "sm510_dualh": ("SM5XX_SM510", "2", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_DUAL_HORIZONTAL", 2),
    "sm511_common": ("SM5XX_SM511", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_SINGLE", 1),
    "sm511_tiger1bit": ("SM5XX_SM511", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_SINGLE", 1),
    "sm511_tiger2bit": ("SM5XX_SM511", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM511_TIGER2BIT", "LCD_LAYOUT_SINGLE", 1),
    "sm511_dualv": ("SM5XX_SM511", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_DUAL_VERTICAL", 2),
    "sm512_dualv": ("SM5XX_SM512", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_DUAL_VERTICAL", 2),
    "sm511_tripleh": ("SM5XX_SM511", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1",
                      "LCD_LAYOUT_TRIPLE_HORIZONTAL", 3),
    "sm530_common": ("SM5XX_SM530", "SM5XX_RMASK_DIRECT", "LCD_WIRING_SM510_S_MUX_R1", "LCD_LAYOUT_SINGLE", 1),
}
BUILTIN_SVG = {"elbaskb": ("lcd_builtin_elbaskb_svg", 338, 499)}
SCREEN_REGIONS = {1: ["screen"], 2: ["screen_top", "screen_bottom", "screen_left", "screen_right"],
                  3: ["screen_left", "screen_middle", "screen_right"]}

KEYCODES = {"ENTER": "RETURN", "ENTER_PAD": "KP_ENTER", "DEL": "DELETE", "BACKSPACE": "BACKSPACE",
            "STOP": "PERIOD", "DEL_PAD": "KP_PERIOD", "PLUS_PAD": "KP_PLUS", "MINUS_PAD": "KP_MINUS",
            "ASTERISK": "KP_MULTIPLY", "SLASH_PAD": "KP_DIVIDE", "SLASH": "SLASH", "LEFT": "LEFT",
            "RIGHT": "RIGHT", "UP": "UP", "DOWN": "DOWN", "SPACE": "SPACE"}


def retrok(code):
    if code is None:
        return "0"
    if re.fullmatch(r"[A-Z]", code):
        return "RETROK_" + code.lower()
    if re.fullmatch(r"[0-9]", code):
        return "RETROK_" + code
    if re.fullmatch(r"[0-9]_PAD", code):
        return "RETROK_KP" + code[0]
    return "RETROK_" + KEYCODES[code] if code in KEYCODES else "0"


PREFERRED = {
    "JOYSTICK_LEFT": "LEFT", "JOYSTICK_RIGHT": "RIGHT", "JOYSTICK_UP": "UP", "JOYSTICK_DOWN": "DOWN",
    "JOYSTICKLEFT_LEFT": "LEFT", "JOYSTICKLEFT_RIGHT": "RIGHT", "JOYSTICKLEFT_UP": "UP", "JOYSTICKLEFT_DOWN": "DOWN",
    "JOYSTICKRIGHT_LEFT": "Y", "JOYSTICKRIGHT_RIGHT": "A", "JOYSTICKRIGHT_UP": "X", "JOYSTICKRIGHT_DOWN": "B",
    "BUTTON1": "A", "BUTTON2": "B", "BUTTON3": "Y", "BUTTON4": "X", "BUTTON5": "L2", "BUTTON6": "R2",
    "START": "START", "START1": "START", "START2": "SELECT", "SELECT": "L", "SERVICE2": "R",
    "POWER_ON": "START", "POWER_OFF": "L2", "VOLUME_DOWN": "R2",
}
FALLBACK = ["L2", "R2", "L3", "X", "Y", "B", "A", "R", "L", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT"]
KEYPAD_PREFERRED = {"Left": "LEFT", "Right": "RIGHT", "Up": "UP", "Down": "DOWN"}
DEFAULT_NAMES = {
    "JOYSTICK_LEFT": "Left", "JOYSTICK_RIGHT": "Right", "JOYSTICK_UP": "Up", "JOYSTICK_DOWN": "Down",
    "JOYSTICKLEFT_LEFT": "Left", "JOYSTICKLEFT_RIGHT": "Right", "JOYSTICKLEFT_UP": "Left Up",
    "JOYSTICKLEFT_DOWN": "Left Down", "JOYSTICKRIGHT_LEFT": "Right Left", "JOYSTICKRIGHT_RIGHT": "Right Right",
    "JOYSTICKRIGHT_UP": "Right Up", "JOYSTICKRIGHT_DOWN": "Right Down", "START": "Start", "START1": "Start",
    "START2": "Start 2", "SELECT": "Select", "SERVICE2": "Alarm", "SERVICE1": "Service", "POWER_ON": "Power On",
    "POWER_OFF": "Power Off", "VOLUME_DOWN": "Sound",
}


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def blocks(src, start_re, end_token):
    out = {}
    for m in re.finditer(start_re, src):
        end = src.index(end_token, m.end())
        out[m.group(1)] = src[m.end():end]
    return out


def add_bit(ports, port, entry, modify):
    """PORT_MODIFY replaces any field overlapping the new one, as MAME does."""
    if modify:
        ports[port] = [b for b in ports[port] if not b[0] & entry[0]]
    ports[port].append(entry)


def parse_ports(body, all_ports, depth=0):
    """Returns {port_name: [(mask, active_low, ipt, name, rest, acl, extra)]} in declaration order.
    extra holds the keycode, the dial flag and PORT_CONDITION details."""
    ports, cur, modify = {}, None, False
    for line in body.splitlines():
        line = line.split("//")[0].strip()
        inc = re.match(r"PORT_INCLUDE\(\s*(\w+)\s*\)", line)
        if inc and depth < 4 and inc.group(1) in all_ports:
            for k, v in parse_ports(all_ports[inc.group(1)], all_ports, depth + 1).items():
                ports.setdefault(k, []).extend(v)
            continue
        st = re.match(r'PORT_(START|MODIFY)\(\s*"([^"]+)"\s*\)', line)
        if st:
            cur = st.group(2)
            modify = st.group(1) == "MODIFY"
            ports.setdefault(cur, [])
            continue
        if cur is None:
            continue
        cn = re.match(r'PORT_(?:CONFNAME|DIPNAME)\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(0x[0-9a-fA-F]+|\d+)', line)
        if cn:
            add_bit(ports, cur, (int(cn.group(1), 0), False, "CONF", None, int(cn.group(2), 0), False, {}), modify)
            continue
        bit = re.match(r"PORT_BIT\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(\w+)\s*,\s*IPT_(\w+)\s*\)(.*)", line)
        if bit:
            mask, act, ipt, rest = int(bit.group(1), 0), bit.group(2), bit.group(3), bit.group(4)
            name = re.search(r'PORT_NAME\(\s*(?:u8)?"([^"]*)"\s*\)', rest)
            code = re.search(r"PORT_CODE\(\s*KEYCODE_(\w+)\s*\)", rest)
            cond = re.search(r'PORT_CONDITION\(\s*"(\w+)"\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', rest)
            extra = {"key": code.group(1) if code else None, "cocktail": "PORT_COCKTAIL" in rest}
            active_low = act == "IP_ACTIVE_LOW"
            if act not in ("IP_ACTIVE_LOW", "IP_ACTIVE_HIGH"):
                active_low = None
                default = int(act, 0)
                if ipt == "CUSTOM" and cond and cond.group(3) == "NOTEQUALS" and int(cond.group(4), 0) == 0 \
                        and default == mask:
                    extra["cond"] = (cond.group(1), int(cond.group(2), 0))
                elif ipt == "DIAL" and default == 0 and "PORT_CODE_DEC(INPUT_CODE_INVALID)" in rest:
                    extra["dial"] = True
            add_bit(ports, cur, (mask, active_low, ipt, name.group(1) if name else None, 0,
                                 "acl_button" in rest, extra), modify)
    return ports


def fold_conditions(pdefs):
    """Two-contact buttons: an IPT_CUSTOM bit that reads its default while (FAKE & m) != 0 is the OR of the
    FAKE buttons in m, so each of those buttons drives the bit directly. Returns None when unsupported."""
    conds = [(port, b[0], b[6]["cond"]) for port, bits in pdefs.items() for b in bits if "cond" in b[6]]
    out = {k: [b for b in v if "cond" not in b[6]] for k, v in pdefs.items() if k not in {c[2][0] for c in conds}}
    for fake in {c[2][0] for c in conds}:
        for b in pdefs.get(fake, []):
            if b[2] in ("UNUSED", "CONF") or b[1] is not False:
                continue
            targets = [(port, mask) for port, mask, (src, cmask) in conds if src == fake and cmask & b[0]]
            if len(targets) != 1:
                return None
            port, mask = targets[0]
            out.setdefault(port, []).append((mask,) + b[1:])
    return out


def write_compatibility(rows, revision):
    clocks = set(re.findall(r'^\{"(\w+)"', open("src/content/rtc_layouts.inc").read(), re.M))
    out = ["# Compatibility", "",
           "- This list covers the machines in MAME %s `hh_sm510.cpp` that LCD Lab runs." % revision,
           "- `tools/mame_registry.py` generates this file.",
           "- Edit the generator, not this file.",
           "- **Clock** marks games that start at the system time.",
           "- [RTC.md](RTC.md) describes the clock.", "",
           "| Set | Title | Year | CPU | Clock | Notes |", "|-----|-------|------|-----|-------|-------|"]
    for name, title, year, cpu, notes in rows:
        out.append("| `%s` | %s | %s | %s | %s | %s |" % (name, title.replace("|", "/"), year, cpu,
                                                         "Yes" if name in clocks else "", notes))
    open("docs/COMPATIBILITY.md", "w").write("\n".join(out) + "\n")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mame = sys.argv[1]
    src = open(mame + "/src/mame/handheld/hh_sm510.cpp").read()
    revision = "0.289"

    systs = re.findall(r"^SYST\(\s*([0-9?]+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,"
                       r"\s*\w+\s*,\s*\"([^\"]*)\"\s*,\s*\"([^\"]*)\"\s*,\s*([^)]*)\)", src, re.M)
    configs = {(a, b): c for a, b, c in
               re.findall(r"void (\w+)_state::(\w+)\(machine_config\s*&\s*config\)\n\{(.*?)\n\}", src, re.S)}
    classes, bases = {}, {}
    for m in re.finditer(r"class (\w+)_state : public (\w+)_state\n\{(.*?)\n\};", src, re.S):
        classes[m.group(1)] = m.group(3)
        bases[m.group(1)] = m.group(2)
    sample_lists = {m.group(1): re.findall(r'"([^"]*)"', m.group(2)) for m in
                    re.finditer(r"static const char \*const (\w+)\[\] =\s*\{(.*?)\};", src, re.S)}
    handlers = {(a, b): c for a, b, c in re.findall(r"\n(?:void|u8|int) (\w+)_state::(\w+)\([^)]*\)\n\{(.*?)\n\}",
                                                    src, re.S)}

    def chain(cls):
        out = []
        while cls in classes:
            out.append(cls)
            cls = bases.get(cls)
        return out
    ports_src = blocks(src, r"static INPUT_PORTS_START\(\s*(\w+)\s*\)", "INPUT_PORTS_END")
    roms = blocks(src, r"ROM_START\(\s*(\w+)\s*\)", "ROM_END")

    entries, skipped, compat = [], [], []
    for year, name, parent, machine, inputs, cls, _company, title, _flags in systs:
        cls = cls.replace("_state", "")
        lineage = chain(cls)
        body = next((configs[(c, machine)] for c in lineage if (c, machine) in configs), None)
        if body is None:
            skipped.append((name, "config not found"))
            continue
        body = re.sub(r"//[^\n]*", "", body)
        inherited = re.match(r"\s*(\w+)\(config\);", body)
        if inherited:
            base_cfg = next((configs[(c, inherited.group(1))] for c in lineage
                             if (c, inherited.group(1)) in configs), None)
            if base_cfg is not None:
                body = re.sub(r"//[^\n]*", "", base_cfg) + body[inherited.end():]
        quirks, samples, builtin = [], None, None
        own_screen = re.search(r'config\.device_remove\("screen"\);\s*mcfg_svg_screen\(config,\s*(\d+),\s*(\d+)\);',
                               body)
        if own_screen:
            body = body.replace(own_screen.group(0), "")
        for names in re.findall(r"m_samples->set_samples_names\((\w+)\);", body):
            samples = sample_lists.get(names)
        if samples and "sound_w" in "".join(classes.get(c, "") for c in lineage) and \
                "S7: ADPCM ST" in handlers.get((lineage[-1], "sound_w"), ""):
            body = re.sub(r"m_maincpu->write_s\(\)\.set\(FUNC\(\w+::sound_w\)\);|"
                          r"m_maincpu->read_b\(\)\.set\(FUNC\(\w+::sound_busy_r\)\);|"
                          r"SAMPLES\(config, m_samples\);|m_samples->[^;]*;", "", body)
            quirks.append("LCD_QUIRK_ADPCM")
        silent = re.search(r"m_maincpu->write_s\(\)\.set\(FUNC\((\w+)_state::(\w+)\)\);", body)
        if silent and not re.sub(r"//[^\n]*|\s", "", handlers.get((silent.group(1), silent.group(2)), "x")):
            body = body.replace(silent.group(0), "")
            quirks.append("SILENT_S")
        if name in BUILTIN_SVG and re.search(r"config\.set_default_layout\(layout_%s\);" % name, body):
            body = re.sub(r"config\.set_default_layout\(layout_\w+\);", "", body)
            builtin = BUILTIN_SVG[name]
            quirks.append("LCD_QUIRK_LED")
        call = re.search(r"\b(" + "|".join(PRESETS) + r")\(config,([^)]*)\)", body)
        if not call:
            first = re.search(r"\b(\w+)\(config", body)
            skipped.append((name, "unsupported preset " + (first.group(1) if first else "?")))
            continue
        preset = call.group(1)
        extra = re.sub(r"\s+", " ", body.replace(call.group(0), "")).strip().strip(";").strip()
        cpu, rmask, wiring, layout, nscreens = PRESETS[preset]
        extra = re.sub(r"m_maincpu->write_r\(\)\.append\(FUNC\(\w+::led_w\)\);?", "", extra).strip()
        if re.fullmatch(r"m_maincpu->write_r\(\)\.set\(FUNC\(\w+::piezo_r2_w\)\);?", extra) and \
                wiring == "LCD_WIRING_SM510_S_MUX_R1":
            wiring, extra = "LCD_WIRING_SM510_S_MUX_R2", ""
        if extra:
            if re.fullmatch(r"m_maincpu->set_r_mask_option\(sm510_base_device::RMASK_DIRECT\);?", extra):
                rmask = "SM5XX_RMASK_DIRECT"
            elif re.fullmatch(r"m_maincpu->set_r_mask_option\((\d+)\);?", extra):
                rmask = re.fullmatch(r"m_maincpu->set_r_mask_option\((\d+)\);?", extra).group(1)
            else:
                skipped.append((name, "custom machine config: " + extra.splitlines()[0][:60]))
                continue
        cbody = "".join(classes.get(c, "") for c in lineage)
        overrides = re.findall(r"virtual \w+ (\w+)\([^)]*\) override;", cbody)
        if overrides == ["input_w"] and "(data & 0x1f) | (data >> 1 & 0x10)" in handlers.get((cls, "input_w"), ""):
            quirks.append("LCD_QUIRK_S5_S6_TIED")
        elif overrides == ["update_display"] and builtin:
            pass
        elif overrides:
            skipped.append((name, "custom handlers in " + cls + "_state"))
            continue
        nums = [int(eval(x)) for x in call.group(2).split(",") if x.strip()]
        sizes = [(nums[i], nums[i + 1]) for i in range(0, len(nums), 2)]
        if own_screen:
            sizes = [(int(own_screen.group(1)), int(own_screen.group(2)))]
        if builtin:
            sizes = [builtin[1:]]

        rb = roms.get(name)
        if rb is None:
            skipped.append((name, "no ROM definition"))
            continue
        loads = re.findall(r'ROM_REGION\(\s*(\w+)\s*,\s*"([\w:]+)"[^)]*\)\s*\n\s*ROM_LOAD\(\s*"([^"]+)"\s*,\s*(\w+)\s*,'
                           r'\s*(\w+)\s*,\s*(?:BAD_DUMP\s+)?CRC\((\w+)\)\s*SHA1\((\w+)\)', rb)
        regions = {r[1]: r for r in loads}
        if "maincpu" not in regions:
            skipped.append((name, "no single program ROM"))
            continue
        mel = regions.get("maincpu:melody")
        if cpu in ("SM5XX_SM511", "SM5XX_SM512", "SM5XX_SM530") and (not mel or int(mel[4], 0) != 0x100):
            skipped.append((name, "no 256 byte melody ROM"))
            continue
        melody = "{%s, 0x100, UINT32_C(0x%s), %s}" % (c_str(mel[2]), mel[5].lower(), c_str(mel[6].lower())) \
            if mel else "{NULL, 0, 0, NULL}"
        main = regions["maincpu"]
        screen_regions = [r for r in SCREEN_REGIONS[nscreens] if r in regions]
        if builtin:
            screen_regions = ["builtin"]
        if len(screen_regions) != nscreens or len(sizes) != nscreens:
            skipped.append((name, "screen definitions do not match the preset"))
            continue

        pdefs = fold_conditions(parse_ports(ports_src.get(inputs, ""), ports_src))
        if pdefs is None or any(b[1] is None and not b[6].get("dial") or b[2] == "CUSTOM"
                                for v in pdefs.values() for b in v):
            skipped.append((name, "inputs need conditions or analogue controls"))
            continue
        lines = sorted(int(k[3:]) for k in pdefs if k.startswith("IN."))
        if lines and lines != list(range(len(lines))) or len(lines) > 9:
            skipped.append((name, "non sequential input lines"))
            continue
        fixed_last = "inp_fixed_last()" in cbody
        if "SILENT_S" in quirks:
            quirks.remove("SILENT_S")
            if len(lines) - (1 if fixed_last else 0) != 0:
                skipped.append((name, "S output replaced while it drives the input mux"))
                continue
        inp_lines = len(lines) - (1 if fixed_last and lines else 0)
        inp_fixed = inp_lines if fixed_last and lines else -1

        rest = [0] * 9
        ba_rest = b_rest = -1
        used, assigned, ok = set(), [], True
        pending = []
        for port, bits in pdefs.items():
            if port.startswith("IN."):
                pidx = "LCD_PORT_IN0 + %d" % int(port[3:])
            elif port in ("BA", "B"):
                pidx = "LCD_PORT_" + port
                if port == "BA":
                    ba_rest = 0
                else:
                    b_rest = 0
            elif port == "ACL":
                pidx = "LCD_PORT_ACL"
            else:
                continue
            for mask, active_low, ipt, pname, conf_rest, acl, extra in bits:
                if ipt == "CONF":
                    if port.startswith("IN."):
                        rest[int(port[3:])] |= conf_rest
                    elif port == "BA":
                        ba_rest |= conf_rest
                    elif port == "B":
                        b_rest |= conf_rest
                    continue
                if active_low:
                    if port.startswith("IN."):
                        rest[int(port[3:])] |= mask
                    elif port == "BA":
                        ba_rest |= mask
                    elif port == "B":
                        b_rest |= mask
                if ipt == "UNUSED":
                    continue
                label = pname or DEFAULT_NAMES.get(ipt) or ipt.replace("_", " ").title()
                if extra.get("cocktail"):
                    label = "P2 " + label
                key = retrok(extra.get("key")) if ipt == "KEYPAD" else "0"
                if ipt == "KEYPAD" and len(label) == 1 and not acl:
                    pending.append(("NONE", pidx, mask, 0, label, key, 0))
                    continue
                if ipt == "KEYPAD" and not acl:
                    want = KEYPAD_PREFERRED.get(label)
                else:
                    want = "R3" if acl else ("L3" if ipt == "SERVICE1" else PREFERRED.get(ipt))
                choice = want if want and want not in used else next((f for f in FALLBACK if f not in used), None)
                if choice is None and key == "0":
                    ok = False
                    break
                if choice:
                    used.add(choice)
                assigned.append((choice or "NONE", pidx, mask, 1 if active_low else 0, label, key,
                                 1 if extra.get("dial") else 0))
        assigned += pending
        if not ok or len(assigned) > 40:
            skipped.append((name, "more inputs than handheld buttons"))
            continue

        decay = [0, 0]
        pv = re.search(r"m_decay_pivot\s*=\s*(\d+)", cbody)
        ln = re.search(r"m_decay_len\s*=\s*(\d+)", cbody)
        if pv:
            decay[0] = int(pv.group(1))
        if ln:
            decay[1] = int(ln.group(1))

        region_size = int(main[0], 0)
        screens = []
        for (w, h), reg in zip(sizes, screen_regions):
            if reg == "builtin":
                screens.append("{{NULL, 0, 0, NULL}, %d, %d}" % (w, h))
                continue
            r = regions[reg]
            screens.append("{{%s, %d, UINT32_C(0x%s), %s}, %d, %d}" % (c_str(r[2]), int(r[4], 0), r[5].lower(),
                                                                       c_str(r[6].lower()), w, h))
        ins = ",\n            ".join("{%s, %s, 0x%02x, %d, %s, %s, %d}" % (
            "LCD_NO_PAD" if a == "NONE" else "J(%s)" % a, p, m, low, c_str(l), k, dial)
            for a, p, m, low, l, k, dial in assigned)
        sample_init = "NULL"
        if samples:
            sample_init = "(const char *const[]){%s, NULL}" % ", ".join(c_str(x) for x in samples[1:])
        notes = [text for flag, text in (("LCD_QUIRK_ADPCM", "voice clips from a MAME sample pack"),
                                         ("LCD_QUIRK_S5_S6_TIED", "tied S5/S6"),
                                         ("LCD_QUIRK_LED", "LED layout built in")) if flag in quirks]
        if any(a == "NONE" for a, *_ in assigned):
            notes.append("keypad keys on the keyboard")
        if any(dial for *_, dial in assigned):
            notes.append("dial on " + next(a for a, *_, dial in assigned if dial))
        compat.append((name, title, year, cpu.replace("SM5XX_", ""), ", ".join(notes)))
        entries.append("""    {
        %s, %s, %s, %s, %s,
        %s, 0x%x,
        {%s, 0x%x, UINT32_C(0x%s), %s},
        %s, %d,
        {%s},
        %d, %d, {%s}, %d, %d,
        {
            %s,
        },
        %s, %d, %d,
        %s,
        %s, %s, %s,
    },""" % (c_str(name), c_str(title), c_str(year), cpu, rmask, wiring, region_size, c_str(main[2]),
              int(main[4], 0), main[5].lower(), c_str(main[6].lower()), layout, nscreens, ", ".join(screens),
              inp_lines, inp_fixed, ", ".join(str(x) for x in rest), ba_rest, b_rest, ins,
              c_str(parent) if parent != "0" else "NULL", decay[0], decay[1], melody,
              " | ".join(quirks) if quirks else "0", sample_init, builtin[0] if builtin else "NULL"))

    out = ["/* Generated by tools/mame_registry.py from MAME %s src/mame/handheld/hh_sm510.cpp" % revision,
           "   (BSD-3-Clause, copyright-holders: hap, Henrik Algestam). Do not edit by hand. */", ""]
    out += entries
    open("src/content/machines_mame.inc", "w").write("\n".join(out) + "\n")
    write_compatibility(compat, revision)
    print("generated %d machines, skipped %d" % (len(entries), len(skipped)))
    if "--report" in sys.argv:
        for n, why in skipped:
            print("  skipped %-12s %s" % (n, why))


if __name__ == "__main__":
    main()
