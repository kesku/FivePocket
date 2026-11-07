#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive vertex-fetch index layouts from the player's extracted shader containers.

Only the five expressions implemented by runtime/vfetch_index_rules.h are emitted.
Unknown arithmetic is tracked conservatively; ambiguous indices or a vertex-index
export without a matching translated VertexIndex input are refused. Output contains
information derived from the player's game and must stay in the private build tree.
"""
from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass
import hashlib
from functools import lru_cache
import json
import math
from pathlib import Path
import re
import struct
import sys

TOOLS = Path(__file__).resolve().parent
sys.path[:0] = [str(TOOLS / 'iter'), str(TOOLS / 'own_disc')]
from generic_shader_pack import canonical_microcode, fnv1a64, parse_container


class Unsupported(ValueError):
    """The shader needs more than the runtime's index-layout contract."""


@dataclass(frozen=True)
class Expr:
    op: str
    args: tuple = ()
    index: bool = False


I = Expr('index', (), True)
UNKNOWN = Expr('unknown')
UNKNOWN_INDEX = Expr('unknown', (), True)


def literal(bits: int) -> Expr:
    return Expr('literal', (bits,))


def float_bits(value: float) -> int:
    return struct.unpack('<I', struct.pack('<f', value))[0]


def value(expr: Expr) -> float | None:
    return struct.unpack('<f', struct.pack('<I', expr.args[0]))[0] if expr.op == 'literal' else None


@lru_cache(maxsize=65536)
def interval(expr: Expr) -> tuple[float, float] | None:
    if expr == I:
        return 0.0, float(1 << 32)
    if expr.op == 'literal':
        n = value(expr)
        return (n, n) if math.isfinite(n) else None
    bounds = [interval(a) for a in expr.args if isinstance(a, Expr)]
    if not bounds or any(b is None for b in bounds):
        return None
    a, b = bounds[0], bounds[-1]
    if expr.op == 'negate':
        result = -a[1], -a[0]
    elif expr.op == 'absolute':
        result = (0.0 if a[0] <= 0 <= a[1] else min(abs(a[0]), abs(a[1])), max(abs(a[0]), abs(a[1])))
    elif expr.op == 'add':
        result = a[0] + b[0], a[1] + b[1]
    elif expr.op == 'mul':
        values = [x * y for x in a for y in b]
        result = min(values), max(values)
    elif expr.op in ('truncate', 'floor', 'fraction'):
        result = (0.0, 1.0) if expr.op == 'fraction' else (math.floor(a[0]), math.ceil(a[1]))
    else:
        return None
    if not all(math.isfinite(x) and abs(x) <= 3.4028234663852886e38 for x in result):
        return None
    rounded = tuple(struct.unpack('<f', struct.pack('<f', x))[0] for x in result)
    return min(result[0], rounded[0]), max(result[1], rounded[1])


def operation(op: str, *args: Expr) -> Expr:
    dependent = any(a.index for a in args)
    if any(a.op == 'unknown' for a in args):
        return UNKNOWN_INDEX if dependent else UNKNOWN
    if op in ('equal', 'not_equal', 'greater', 'greater_equal'):
        a, b = interval(args[0]), interval(args[1])
        if a is not None and b is not None:
            if args[0] == args[1]:
                return literal(float_bits(op in ('equal', 'greater_equal')))
            if op == 'greater_equal' and (a[0] >= b[1] or a[1] < b[0]):
                return literal(float_bits(a[0] >= b[1]))
            if op == 'greater' and (a[0] > b[1] or a[1] <= b[0]):
                return literal(float_bits(a[0] > b[1]))
            if op in ('equal', 'not_equal') and (a[1] < b[0] or b[1] < a[0]):
                return literal(float_bits(op == 'not_equal'))
    if op in ('select_eq', 'select_ge', 'select_gt'):
        condition, yes, no = args
        if yes == no:
            return yes
        bounds = interval(condition)
        if bounds is not None:
            lo, hi = bounds
            if op == 'select_eq' and (lo == hi == 0 or lo > 0 or hi < 0):
                return yes if lo == hi == 0 else no
            if op == 'select_ge' and (lo >= 0 or hi < 0):
                return yes if lo >= 0 else no
            if op == 'select_gt' and (lo > 0 or hi <= 0):
                return yes if lo > 0 else no
    if op in ('maximum', 'minimum') and args[0] == args[1]:
        return args[0]
    if op == 'negate' and args[0].op == 'negate':
        return args[0].args[0]
    numbers = [value(a) for a in args]
    if all(n is not None and math.isfinite(n) for n in numbers):
        try:
            result = {'add': lambda: numbers[0] + numbers[1], 'mul': lambda: numbers[0] * numbers[1],
                      'negate': lambda: -numbers[0], 'absolute': lambda: abs(numbers[0]),
                      'maximum': lambda: max(numbers), 'minimum': lambda: min(numbers),
                      'truncate': lambda: math.trunc(numbers[0]), 'floor': lambda: math.floor(numbers[0]),
                      'fraction': lambda: numbers[0] - math.floor(numbers[0]),
                      'reciprocal': lambda: 1.0 / numbers[0],
                      'saturate': lambda: min(1.0, max(0.0, numbers[0]))}[op]()
            return literal(float_bits(result))
        except (KeyError, OverflowError, ValueError, ZeroDivisionError):
            return UNKNOWN
    if op == 'mul':
        if numbers[0] == 1.0:
            return args[1]
        if numbers[1] == 1.0:
            return args[0]
    if op == 'add':
        if numbers[0] == 0.0:
            return args[1]
        if numbers[1] == 0.0:
            return args[0]
    return Expr(op, args, dependent)


def merge(a: Expr, b: Expr, condition: Expr = UNKNOWN) -> Expr:
    return a if a == b else (UNKNOWN_INDEX if a.index or b.index or condition.index else UNKNOWN)


def guarded(yes: Expr, no: Expr, guard: tuple[Expr, bool]) -> Expr:
    if yes == no:
        return yes
    predicate, expected = guard
    return Expr('guard', (predicate, expected, yes, no), predicate.index or yes.index or no.index)


def restricted(expr: Expr, guards: list[tuple[Expr, bool]]) -> Expr:
    if expr.op == 'guard':
        predicate, expected, yes, no = expr.args
        for known, wanted in guards:
            if predicate == known:
                return restricted(yes if wanted == expected else no, guards)
        return guarded(restricted(yes, guards), restricted(no, guards), (predicate, expected))
    if expr.op in ('index', 'fetch', 'literal', 'unknown', 'predicate'):
        return expr
    return operation(expr.op, *(restricted(a, guards) for a in expr.args))


def float_definitions(container: bytes) -> dict[tuple[int, int], int]:
    """ShaderContainer/DefinitionTable, as specified by XenosRecomp shader.h."""
    if len(container) < 36:
        raise ValueError('short shader container')
    virtual, physical, offset = struct.unpack_from('>II8xI', container, 4)
    if virtual + physical > len(container):
        raise ValueError('container memory extent')
    if not offset:
        return {}
    if offset % 4 or offset + 20 > virtual:
        raise ValueError('float definition table extent')
    cursor, result, records = offset + 20, {}, 0
    while True:
        if cursor + 4 > virtual:
            raise ValueError('unterminated float definitions')
        if struct.unpack_from('>I', container, cursor)[0] == 0:
            return result
        if cursor + 8 > virtual or records >= 4096:
            raise ValueError('float definition record extent')
        reg, components, physical_offset = struct.unpack_from('>HHI', container, cursor)
        size = ((components + 3) // 4) * 16
        if not components or reg + (components + 3) // 4 > 256 or physical_offset % 4 or physical_offset + size > physical:
            raise ValueError('float definition values extent')
        for index, bits in enumerate(struct.unpack_from('>' + 'I' * (size // 4), container, virtual + physical_offset)):
            key = reg + index // 4, index % 4
            if key in result:
                raise ValueError('overlapping float definitions')
            result[key] = bits
        cursor += 8
        records += 1


EXEC = {1, 2, 3, 4, 5, 6, 13, 14}
CONDITIONAL = {3, 4, 5, 6, 13, 14}
END = {2, 4, 6, 14}


def controls(code: bytes) -> tuple[tuple[int, ...], list[tuple[int, int]]]:
    if not code or len(code) % 12 or len(code) > 1 << 20:
        raise Unsupported('microcode extent')
    words = struct.unpack('>' + 'I' * (len(code) // 4), code)
    limit, slot, result = len(code) // 12, 0, []
    while slot < limit:
        a, b, c = words[slot * 3:slot * 3 + 3]
        for cf in (a | ((b & 65535) << 32), (b >> 16) | (c << 16)):
            op = cf >> 44
            if op in EXEC:
                address, count, sequence = cf & 4095, (cf >> 12) & 7, (cf >> 16) & 4095
                if count > 6 or address + count > len(code) // 12 or sequence >> (2 * count):
                    raise Unsupported('EXEC extent or sequence')
                if count:
                    limit = min(limit, address)
            elif op not in (0, 11, 12, 15):
                raise Unsupported('loop/call control flow')
            result.append((op, cf))
        slot += 1
        if len(result) > 512:
            raise Unsupported('control-flow budget')
    if slot > limit or not any(op in END for op, _ in result):
        raise Unsupported('control/instruction overlap or missing termination')
    for op, cf in result:
        if op == 11 and (cf & 8191) >= len(result):
            raise Unsupported('jump target outside control flow')
    return words, result


@dataclass(frozen=True)
class State:
    registers: tuple[Expr, ...]
    scalar: Expr = UNKNOWN
    full_fetch: Expr = UNKNOWN
    predicate: Expr = UNKNOWN
    exports: tuple[Expr, ...] = (UNKNOWN,) * 256
    address: Expr = UNKNOWN

    def joined(self, other: State, condition: Expr = UNKNOWN) -> State:
        return State(tuple(merge(a, b, condition) for a, b in zip(self.registers, other.registers)),
                     merge(self.scalar, other.scalar, condition), merge(self.full_fetch, other.full_fetch, condition),
                     merge(self.predicate, other.predicate, condition),
                     tuple(merge(a, b, condition) for a, b in zip(self.exports, other.exports)),
                     merge(self.address, other.address, condition))

    def under_guard(self, other: State, guard: tuple[Expr, bool]) -> State:
        return State(tuple(guarded(a, b, guard) for a, b in zip(self.registers, other.registers)),
                     guarded(self.scalar, other.scalar, guard), guarded(self.full_fetch, other.full_fetch, guard),
                     guarded(self.predicate, other.predicate, guard),
                     tuple(guarded(a, b, guard) for a, b in zip(self.exports, other.exports)),
                     guarded(self.address, other.address, guard))


@dataclass(frozen=True)
class Fetch:
    address: int
    destination: int
    mask: int
    source: int
    component: int
    expression: Expr


class Analyzer:
    def __init__(self, code: bytes, constants: dict[tuple[int, int], int], allow_predicated: bool = False):
        self.words, self.cf = controls(code)
        self.allow_predicated = allow_predicated
        self.constants = constants
        self.read_literals: set[tuple[int, int]] = set()
        addresses = set()
        for op, cf in self.cf:
            if op in EXEC:
                for i in range((cf >> 12) & 7):
                    at = (cf & 4095) + i
                    if ((cf >> (16 + 2 * i)) & 1) and self.words[at * 3] & 31 == 0:
                        addresses.add(at)
        self.addresses = sorted(addresses)
        if len(addresses) > 12:
            raise Unsupported('more than twelve vertex fetches')
        self.fetch_ids = {address: i for i, address in enumerate(self.addresses)}
        self.fetches: dict[int, Fetch] = {}

    def constant(self, register: int, component: int, relative: bool, address: Expr = UNKNOWN) -> Expr:
        if relative:
            return UNKNOWN_INDEX if address.index else UNKNOWN
        key = register, component
        if key not in self.constants:
            return UNKNOWN
        self.read_literals.add(key)
        return literal(self.constants[key])

    def clause(self, state: State, cf: int) -> State:
        registers = list(state.registers)
        previous, full_fetch, predicate = state.scalar, state.full_fetch, state.predicate
        exports = list(state.exports)
        address_register = state.address
        clause_guard = (state.predicate, bool(cf & (1 << 42))) if cf >> 44 in (5, 6, 13, 14) else None
        for i in range((cf >> 12) & 7):
            address = (cf & 4095) + i
            a, b, c = self.words[address * 3:address * 3 + 3]
            if (cf >> (16 + 2 * i)) & 1:
                source, component, destination = (a >> 5) & 63, a >> 30, (a >> 12) & 63
                fetch_guard = (predicate, bool(c & (1 << 31))) if b & (1 << 31) else None
                active = [g for g in (clause_guard, fetch_guard) if g is not None]
                if a & 31:
                    # Texture reads are not repaired by a vertex-buffer layout.
                    dep = any(registers[source * 4 + lane].index for lane in range(4))
                    fetched = UNKNOWN_INDEX if dep else UNKNOWN
                else:
                    if a & ((1 << 11) | (1 << 18)) or b & (1 << 15) or (b & (1 << 31) and not self.allow_predicated):
                        raise Unsupported('relative, rounded or predicated vertex fetch')
                    expression = restricted(full_fetch if b & (1 << 30) else registers[source * 4 + component], active)
                    if not b & (1 << 30):
                        full_fetch = guarded(expression, full_fetch, fetch_guard) if fetch_guard is not None else expression
                    mask = sum(1 << lane for lane in range(4) if ((b >> (3 * lane)) & 7) <= 5)
                    fetch = Fetch(address, destination, mask, source, component, expression)
                    old = self.fetches.get(address)
                    if old and old.expression != expression:
                        fetch = Fetch(address, destination, mask, source, component, merge(old.expression, expression))
                    self.fetches[address] = fetch
                    fetched = None
                for lane in range(4):
                    select = (b >> (3 * lane)) & 7
                    if select == 6:
                        raise Unsupported('reserved fetch destination swizzle')
                    if select < 4:
                        result = fetched if fetched is not None else Expr('fetch', (self.fetch_ids[address], lane))
                        at = destination * 4 + lane
                        registers[at] = guarded(result, registers[at], fetch_guard) if fetch_guard is not None else result
                    elif select < 6:
                        at = destination * 4 + lane
                        result = literal(float_bits(select - 4))
                        registers[at] = guarded(result, registers[at], fetch_guard) if fetch_guard is not None else result
                continue
            if a & (1 << 6) or (a & (1 << 14) and not a & (1 << 15)):
                raise Unsupported('relative ALU destination')
            old = registers.copy()
            guard = (predicate, bool(b & (1 << 27))) if b & (1 << 28) else None
            active = [g for g in (clause_guard, guard) if g is not None]
            address_unknown = address_register if b & (1 << 29) else UNKNOWN

            def source(operand: int, lane: int) -> Expr:
                reg, swizzle = (c >> (16 - operand * 8)) & 255, (b >> (16 - operand * 8)) & 255
                component = ((swizzle >> (lane * 2)) + lane) & 3
                if c & (1 << (31 - operand)):
                    result = (UNKNOWN_INDEX if address_unknown.index or any(v.index for v in old) else UNKNOWN) if reg & 64 else old[(reg & 63) * 4 + component]
                    if reg & 128:
                        result = operation('absolute', result)
                else:
                    earlier = any(not c & (1 << (31 - j)) for j in range(operand))
                    result = self.constant(reg, component, bool(b & (1 << (30 if earlier else 31))), address_unknown)
                    if a & (1 << 7):
                        result = operation('absolute', result)
                result = restricted(result, active)
                return operation('negate', result) if b & (1 << (26 - operand)) else result

            def scalar_source(lane: int) -> Expr:
                reg = c & 255
                if c & (1 << 29) and reg & 64:
                    return UNKNOWN_INDEX if address_unknown.index or any(v.index for v in old) else UNKNOWN
                result = old[(reg & 63) * 4 + lane] if c & (1 << 29) and not reg & 64 else self.constant(reg, lane, bool(b & (3 << 30)), address_unknown)
                if c & (1 << 29) and reg & 128:
                    result = operation('absolute', result)
                result = restricted(result, active)
                return operation('negate', result) if b & (1 << 24) else result

            scalar_op = a >> 26
            left, right = scalar_source((((b & 255) >> 6) + 3) & 3), scalar_source(b & 3)
            scalar = previous
            if 42 <= scalar_op <= 47:
                constant = self.constant(c & 255, (((b & 255) >> 6) + 3) & 3, bool(b & (3 << 30)), address_unknown)
                reg = (scalar_op & 1) | (((c >> 29) & 1) << 1) | (b & 60)
                temporary = restricted(old[reg * 4 + (b & 3)], active)
                if b & (1 << 24):
                    constant, temporary = operation('negate', constant), operation('negate', temporary)
                scalar = operation('mul' if scalar_op < 44 else 'add', constant,
                                   operation('negate', temporary) if scalar_op >= 46 else temporary)
            elif scalar_op != 50:
                if scalar_op in (0, 2, 5, 6, 23, 24, 25):
                    scalar = operation({0: 'add', 2: 'mul', 5: 'maximum', 6: 'minimum', 23: 'maximum', 24: 'maximum', 25: 'add'}[scalar_op], left,
                                       operation('negate', right) if scalar_op == 25 else right)
                elif scalar_op in (1, 3):
                    scalar = operation('add' if scalar_op == 1 else 'mul', left, previous)
                elif scalar_op in (8, 9, 10):
                    scalar = operation({8: 'greater', 9: 'greater_equal', 10: 'not_equal'}[scalar_op], left, literal(0))
                elif scalar_op in (11, 12, 13, 19):
                    scalar = operation({11: 'fraction', 12: 'truncate', 13: 'floor', 19: 'reciprocal'}[scalar_op], left)
                else:
                    scalar = UNKNOWN_INDEX if left.index else UNKNOWN
                if scalar_op in (23, 24):
                    address_register = guarded(left, address_register, guard) if guard is not None else left
                if 27 <= scalar_op <= 34:
                    changed_predicate = Expr('predicate', (address, left, right), left.index or right.index)
                    predicate = guarded(changed_predicate, predicate, guard) if guard is not None else changed_predicate
            vector_mask, scalar_mask = (a >> 16) & 15, (a >> 20) & 15
            export = bool(a & (1 << 15))
            vector_mask = vector_mask & ~scalar_mask if export else vector_mask
            actual_scalar = scalar_mask & ~((a >> 16) & 15) if export else scalar_mask
            op = (c >> 24) & 31
            vectors = [[source(j, lane) for lane in range(4)] for j in range(3)]
            dot = None
            if op in (15, 16, 17):
                for lane in range({15: 4, 16: 3, 17: 2}[op]):
                    product = operation('mul', vectors[0][lane], vectors[1][lane])
                    dot = product if dot is None else operation('add', dot, product)
                if op == 17:
                    dot = operation('add', dot, vectors[2][0])
            for lane in range(4):
                if vector_mask & (1 << lane):
                    x, y, z = (v[lane] for v in vectors)
                    if op in (0, 1, 2, 3):
                        result = operation({0: 'add', 1: 'mul', 2: 'maximum', 3: 'minimum'}[op], x, y)
                    elif op in (4, 5, 6, 7):
                        result = operation({4: 'equal', 5: 'greater', 6: 'greater_equal', 7: 'not_equal'}[op], x, y)
                    elif op in (12, 13, 14):
                        result = operation({12: 'select_eq', 13: 'select_ge', 14: 'select_gt'}[op], x, y, z)
                    elif op in (8, 9, 10):
                        result = operation({8: 'fraction', 9: 'truncate', 10: 'floor'}[op], x)
                    elif op == 11:
                        result = operation('add', operation('mul', x, y), z)
                    elif dot is not None:
                        result = dot
                    else:
                        operands = (x, y, z) if op in (12, 13, 14) else (x, y)
                        result = y if op in (12, 13, 14) and y == z else (UNKNOWN_INDEX if any(v.index for v in operands) else UNKNOWN)
                    if a & (1 << 24):
                        result = operation('saturate', result)
                    if export:
                        at = (a & 63) * 4 + lane
                        exports[at] = guarded(result, exports[at], guard) if guard is not None else result
                    else:
                        at = (a & 63) * 4 + lane
                        registers[at] = guarded(result, old[at], guard) if guard is not None else result
                if actual_scalar & (1 << lane):
                    result = operation('saturate', scalar) if a & (1 << 25) else scalar
                    if export:
                        at = (a & 63) * 4 + lane
                        exports[at] = guarded(result, exports[at], guard) if guard is not None else result
                    else:
                        at = ((a >> 8) & 63) * 4 + lane
                        registers[at] = guarded(result, old[at], guard) if guard is not None else result
            if export:
                for lane in range(4):
                    bit = 1 << lane
                    if ((a >> 16) & 15) & scalar_mask & bit:
                        at = (a & 63) * 4 + lane
                        result = literal(float_bits(1.0))
                        exports[at] = guarded(result, exports[at], guard) if guard is not None else result
                    elif a & (1 << 14) and not (((a >> 16) & 15) | scalar_mask) & bit:
                        at = (a & 63) * 4 + lane
                        result = literal(float_bits(0.0))
                        exports[at] = guarded(result, exports[at], guard) if guard is not None else result
            if op in (20, 21, 22, 23):
                values = tuple(x for vector in vectors[:2] for x in vector)
                changed_predicate = Expr('predicate', (address, *values), any(x.index for x in values))
                predicate = guarded(changed_predicate, predicate, guard) if guard is not None else changed_predicate
            previous = guarded(scalar, previous, guard) if guard is not None else scalar
        return State(tuple(registers), previous, full_fetch, predicate, tuple(exports), address_register)

    def run(self) -> tuple[list[Fetch], list[tuple[int, int, int]], bool]:
        registers = [UNKNOWN] * 256
        registers[0] = I
        states = {0: State(tuple(registers))}
        queue = deque([0])
        iterations = 0
        exits = {}
        while queue:
            pc = queue.popleft()
            if pc >= len(self.cf):
                continue
            iterations += 1
            if iterations > 8192:
                raise Unsupported('control-flow convergence budget')
            state = states[pc]
            op, cf = self.cf[pc]
            after = self.clause(state, cf) if op in EXEC else state
            if op in END:
                exits[pc] = after
            edges = []
            if op == 11:
                edges.append((cf & 8191, after))
                if not cf & (1 << 13):
                    edges.append((pc + 1, after))
            else:
                if op in (5, 13):
                    edges.append((pc + 1, after.under_guard(state, (state.predicate, bool(cf & (1 << 42))))))
                else:
                    if op not in END:
                        edges.append((pc + 1, after))
                    if op in CONDITIONAL:
                        edges.append((pc + 1, state))
            for dest, incoming in edges:
                joined = incoming if dest not in states else states[dest].joined(incoming)
                if states.get(dest) != joined:
                    states[dest] = joined
                    queue.append(dest)
        if len(self.fetches) != len(self.addresses):
            raise Unsupported('unreachable or missing vertex fetch')
        needed = set()
        def constants_in(expression):
            if expression.op == 'literal':
                needed.add(expression.args[0])
            else:
                for arg in expression.args:
                    if isinstance(arg, Expr): constants_in(arg)
        for fetch in self.fetches.values(): constants_in(fetch.expression)
        literals = [(r, c, self.constants[r, c]) for r, c in sorted(self.read_literals) if self.constants[r, c] in needed]
        if len(literals) > 8:
            raise Unsupported('more than eight referenced shader literals')
        return [self.fetches[a] for a in self.addresses], literals, any(v.index for state in exits.values() for v in state.exports)


def scaled(expr: Expr, form: str) -> tuple[Expr, int] | None:
    if expr.op == form:
        return expr, float_bits(1.0)
    if expr.op == 'mul':
        for a, b in (expr.args, expr.args[::-1]):
            if a.op == form and b.op == 'literal' and math.isfinite(value(b)):
                return a, b.args[0]
    return None


def form(expr: Expr) -> tuple[str, int, int, int, int]:
    direct = scaled(expr, 'index')
    if direct:
        return ('Index' if expr == I else 'Scale', 0 if expr == I else direct[1], 0, 255, 0)
    remap = scaled(expr, 'fetch')
    if remap:
        fetch, k = remap
        return 'Remap', k, 0, *fetch.args
    fraction = scaled(expr, 'fraction')
    if fraction:
        frac, n = fraction
        inner = scaled(frac.args[0], 'index')
        if inner:
            return 'Fmod', inner[1], n, 255, 0
    if expr.op == 'add':
        for initial, subtract in (expr.args, expr.args[::-1]):
            if initial != I or subtract.op != 'mul':
                continue
            for trunc, n in (subtract.args, subtract.args[::-1]):
                if trunc.op == 'negate':
                    trunc = trunc.args[0]
                    multiplier = n
                elif n.op == 'literal':
                    multiplier = operation('negate', n)
                else:
                    continue
                if trunc.op == 'truncate' and multiplier.op == 'literal':
                    inner = scaled(trunc.args[0], 'index')
                    if inner:
                        return 'Mod', inner[1], multiplier.args[0], 255, 0
    raise Unsupported('index expression is outside Index/Scale/Mod/Fmod/Remap')


def derive(code: bytes, constants: dict, locations: dict[int, int], *, vertex_index: bool = False, allow_predicated: bool = False) -> dict | None:
    analyzer = Analyzer(code, constants, allow_predicated)
    fetches, literals, exports_index = analyzer.run()
    if exports_index and not vertex_index:
        raise Unsupported('translated module does not provide exported vertex-index arithmetic')
    rows = []
    for fetch in fetches:
        expression = form(fetch.expression)
        if expression[0] == 'Remap' and expression[3] >= len(rows):
            raise Unsupported('remap is not an earlier vertex fetch')
        rows.append({'address': fetch.address, 'destination': fetch.destination, 'mask': fetch.mask,
                     'source': fetch.source, 'component': fetch.component, 'form': expression[0],
                     'k': expression[1], 'n': expression[2], 'via': expression[3], 'via_component': expression[4],
                     'location': locations.get(fetch.address, 255)})
    if not any(r['form'] != 'Index' for r in rows):
        return None
    by_location = {}
    for row in rows:
        location = row['location']
        if location == 255:
            continue
        index = tuple(row[k] for k in ('form', 'k', 'n', 'via', 'via_component'))
        if location in by_location and by_location[location] != index:
            raise Unsupported('translated input aliases different computed fetch indices')
        by_location[location] = index
    return {'fetches': rows, 'literals': literals}


def emit(rules: dict[int, dict]) -> str:
    lines = ['// Generated from the player\'s shader containers by tools/vfi_derive.py. Do not redistribute.']
    for key, rule in sorted(rules.items()):
        fetches = ','.join('{' + f"{r['address']},{r['destination']},0x{r['mask']:x},{r['source']},{r['component']},Form::{r['form']},0x{r['k']:08x}u,0x{r['n']:08x}u,{r['via']},{r['via_component']},{r['location']}" + '}' for r in rule['fetches'])
        literals = ','.join('{' + f'{r},{c},0x{bits:08x}u' + '}' for r, c, bits in rule['literals'])
        lines.append('{' + f'0x{key:016x}ull,{len(rule["fetches"])},{{{fetches}}},{len(rule["literals"])},{{{literals}}}' + '},')
    return '\n'.join(lines) + '\n'


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--pack', type=Path)
    parser.add_argument('--shader-fixes', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args(argv)
    from build_shader_pack import validate_inventory
    from build_shader_fixes import pack_entries, table_modules, input_locations, builtins
    import host_support
    host_support.private_output(args.out)
    if args.report:
        host_support.private_output(args.report)
    _, entries = validate_inventory(args.inventory)
    packed = pack_entries(args.pack.read_bytes()) if args.pack else {}
    overrides, extra_locations = {}, {}
    if args.shader_fixes:
        for kind in ('ptx', 'cloth'):
            path = args.shader_fixes / f'generic_{kind}_overrides.inc'
            if not path.is_file():
                raise ValueError(f'missing {kind} shader-fix table')
            text = path.read_text()
            overrides.update(table_modules(text))
            if kind == 'cloth':
                for key, a, x, b, y in re.findall(r'\{0x([0-9a-f]{16})ull,\d+u,\w+,0x[0-9a-f]{16}ull,\{\{(\d+)u,(\d+)u\},\{(\d+)u,(\d+)u\}\}\}', text):
                    extra_locations[int(key, 16)] = {int(a): int(x), int(b): int(y)}
    if set(extra_locations) - overrides.keys():
        raise ValueError('cloth location metadata has no matching override module')
    rules, seen, rejected = {}, set(), []
    considered = 0
    for entry in entries:
        if entry['stage'] != 'vertex':
            continue
        key = int(entry['key'], 16)
        if key in seen or (args.pack and key not in packed):
            continue
        seen.add(key)
        considered += 1
        data = (args.inventory / entry['file']).read_bytes()
        code, elements, _, _ = parse_container(data, 'vertex')
        if fnv1a64(canonical_microcode(code)) != key:
            raise ValueError('container key changed during derivation')
        container_locations = {address: location for address, _, _, location, _ in elements}
        locations = dict(container_locations)
        module = overrides.get(key, packed[key][1] if key in packed else None)
        locations.update(extra_locations.get(key, {}))
        if module is not None and not set(extra_locations.get(key, {}).values()).issubset(input_locations(module)):
            raise ValueError('shader-fix locations are absent from the override module')
        try:
            rule = derive(code, float_definitions(data), locations, vertex_index=module is not None and 42 in builtins(module), allow_predicated=key in extra_locations)
        except Unsupported as error:
            rejected.append({'key': entry['key'], 'reason': str(error)})
            continue
        if rule is not None:
            # The runtime associates fetches by address. Keep original container metadata in the
            # informational location field while verifying alias safety against final override inputs.
            for row in rule['fetches']:
                row['location'] = container_locations.get(row['address'], 255)
            rules[key] = rule
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(emit(rules))
    report = {'schema': 'vertex-fetch-index-rules/1', 'considered': considered, 'rules': len(rules),
              'rejected': rejected, 'output_sha256': hashlib.sha256(args.out.read_bytes()).hexdigest()}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Vertex index layouts: {len(rules)} rules from {considered} vertex shaders; {len(rejected)} conservatively excluded.')
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f'vertex-fetch derivation refused: {error}', file=sys.stderr)
        raise SystemExit(2)
