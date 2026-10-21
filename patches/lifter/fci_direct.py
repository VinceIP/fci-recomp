"""Bounded recovery of direct-branch targets inside a function, without splitting functions.

Copied to tools/recomp/fci_direct.py in the patched lifter (patches/lifter/0003). Independently callable
entries are never removed; unknown or overlapping streams stay unresolved, and existing instructions are
kept as decoded.
"""


def table_arms(translator, insn, keep):
    """a `jmp [reg*4 + table]`'s arms: its table's entries up to the first that keep(target, table) refuses"""
    ops = insn.operands
    if not insn.is_jump or len(ops) != 1 or ops[0].type != "mem" or not ops[0].mem_index or not ops[0].mem_disp:
        return []
    table = ops[0].mem_disp
    arms = []
    for target in translator.lifter._read_jump_table(table):
        if not keep(target, table):
            break
        arms.append(target)
    return arms if len(arms) >= 2 else []


def recover_direct(translator, start, end, instructions, blocks):
    from . import config

    lifter = translator.lifter
    lifter.fci_local_addresses = None
    translator._fci_direct_extended = False
    decoded = {i.address: i for i in instructions}
    section = next((s for s in config._SECTIONS if s.is_code and s.va <= start < s.va + s.raw_size), None)
    if not section or not decoded:
        return instructions, blocks
    lo = max(section.va, start - 0x10000)
    hi = min(section.va + section.raw_size, start + 0x10000)
    pending = [
        i.jump_target
        for i in instructions
        if i.is_branch
        and i.jump_target is not None
        and i.jump_target not in decoded
        and i.jump_target not in translator.func_db
    ]
    if not pending:
        return instructions, blocks
    attempted = set()
    added = 0
    # Ownership is per instruction, never inferred from the numerical span.
    occupied = {a for i in instructions for a in range(i.address, i.end_address)}
    while pending:
        target = pending.pop()
        if target in decoded or target in translator.func_db or target in attempted:
            continue
        attempted.add(target)
        if not (lo <= target < hi) or target in occupied:
            continue
        raw = translator._read_func_bytes(target, hi)
        if not raw:
            continue
        stream = []
        valid = False
        for cs in translator.disasm._cs.disasm(raw, target):
            a = cs.address
            if a in decoded or a in translator.func_db:
                valid = bool(stream)
                break
            insn = translator.disasm._decode_instruction(cs)
            if (
                insn.end_address > hi
                or added + len(stream) >= 4096
                or any(b in occupied for b in range(a, insn.end_address))
            ):
                break
            if insn.mnemonic in ("int3", "ud2", "hlt", "iret", "iretd"):
                break
            stream.append(insn)
            if insn.is_ret or insn.is_jump:
                valid = True
                break
        if not valid:
            continue
        span_end = max(max(i.end_address for i in decoded.values()), stream[-1].end_address)
        if span_end - min(min(decoded), target) > 0x10000:
            continue
        for insn in stream:
            decoded[insn.address] = insn
            occupied.update(range(insn.address, insn.end_address))
            if (
                insn.is_branch
                and insn.jump_target is not None
                and insn.jump_target not in decoded
                and insn.jump_target not in translator.func_db
            ):
                pending.append(insn.jump_target)
            # arms to decode: between the function start and the table, where MSVC parks it after the code
            arms = table_arms(translator, insn, lambda t, table: start <= t < table and t not in translator.func_db)
            pending.extend(a for a in arms if a not in decoded)
        added += len(stream)
    if not added:
        return instructions, blocks
    # Reject the transaction if an added stream would fall into an unknown gap.
    for insn in decoded.values():
        if (
            not insn.is_terminator
            and insn.mnemonic not in ("int3", "ud2", "hlt", "iret", "iretd")
            and insn.end_address not in decoded
            and insn.end_address not in translator.func_db
        ):
            return instructions, blocks
    ordered = [decoded[a] for a in sorted(decoded)]
    lower, upper = ordered[0].address, max(i.end_address for i in ordered)
    leaders = {start}
    for left, right in zip(ordered, ordered[1:]):
        if left.end_address != right.address:
            leaders.add(right.address)
    # Keep existing switch/computed-jump leaders from the original decode.
    leaders.update(bb.start for bb in blocks)
    # A switch arm is a label even where the code before it runs straight into it (the arms the lifter takes:
    # patches/lifter/0007, _leading_arms).
    for insn in ordered:
        leaders.update(table_arms(translator, insn, lambda t, table: t in decoded))
    rebuilt = translator.disasm.build_basic_blocks(
        ordered, lower, upper, extra_leaders=leaders, stop_mnemonics=("int3", "ud2", "hlt", "iret", "iretd")
    )
    # Restore original computed-jump edges after repartitioning direct streams.
    original_edges = {bb.last_insn.address: bb.successors for bb in blocks if bb.last_insn}
    for bb in rebuilt:
        for target in original_edges.get(bb.last_insn.address, ()):
            if target in decoded and target not in bb.successors:
                bb.successors.append(target)
    lifter.fci_local_addresses = set(decoded)
    translator._fci_direct_extended = True
    translator.fci_direct_records = getattr(translator, "fci_direct_records", {})
    translator.fci_direct_records[start] = {
        "added_instructions": added,
        "destinations": sorted(attempted & set(decoded)),
        "span": [lower, upper],
    }
    return ordered, rebuilt
