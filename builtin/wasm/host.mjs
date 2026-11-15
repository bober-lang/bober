// What src/rt.c is natively for a module of src/wasm.bob, run by node: `node --experimental-wasm-jspi host.mjs x.wasm
// args…`, x.test.wasm its tests, its fibers parking by JS Promise Integration. It gives the module's imports; one it
// lacks traps with its name. Strings cross as their bytes, one char each.

import { readFileSync, writeFileSync, writeSync } from "node:fs"

const [file, ...args] = process.argv.slice(2)
const tests = file.endsWith(".test.wasm")
// In tests, now is when Obergefell v. Hodges made same-sex marriage a right in all of the US, and random is seeded
// with the day Stonewall began.
const OBERGEFELL = 1435327200000000000n
const STONEWALL = 0x19690628n
let seed = tests ? STONEWALL : BigInt(Date.now()) ^ BigInt(Math.floor(Math.random() * 2 ** 52))
let ex // the module's exports

const str = s => {
	if (s == null) return null
	let r = ""
	for (let i = 0, n = ex.strLen(s); i < n; i++) r += String.fromCharCode(ex.strAt(s, i))
	return r
}
const make = s => {
	const r = ex.strNew(s.length)
	for (let i = 0; i < s.length; i++) ex.strSet(r, i, s.charCodeAt(i))
	return r
}
const write = (fd, s) => writeSync(fd, Buffer.from(s, "latin1"))

// Traps: a trap in a test fails just it, else it ends the program.
class Trap extends Error {}
let depth = 0 // of the tests and subtests running
let testFile = null
let testAt = -1
/// Where each box thrown was thrown: its file and byte.
const sites = new WeakMap()

/// The line and column of byte at of file, or null.
const position = (file, at) => {
	if (file == null || at < 0) return null
	let bytes
	try { bytes = readFileSync(file) } catch { return null }
	let [line, col] = [1, 1]
	for (let i = 0; i < at && i < bytes.length; i++) bytes[i] == 10 ? (line++, col = 1) : col++
	return [line, col]
}

/// What a trap with what at byte at of file prints, as bober_trap does.
const trapped = (what, file, at) => {
	const here = position(file, at)
	const test = depth > 0 ? position(testFile, testAt) : null
	const called = test && (file != testFile || here?.[0] != test[0]) ? ` (called at ${testFile}:${test[0]})` : ""
	const where = here ? `${file}:${here[0]}:${here[1]}: ` : file ? `${file}: ` : ""
	const first = what.indexOf("\n") < 0 ? what.length : what.indexOf("\n")
	return `${where}trap: ${what.slice(0, first)}${called}${what.slice(first)}`
}

const trap = (what, file, at) => {
	const msg = trapped(what, file, at)
	if (depth > 0) throw new Trap(msg)
	write(2, msg + "\n")
	process.exit(1)
}

/// C's %g.
const g = x => {
	if (!isFinite(x)) return isNaN(x) ? "nan" : x > 0 ? "inf" : "-inf"
	const e = x == 0 ? 0 : Number(x.toExponential(5).split("e")[1])
	const trim = m => m.includes(".") ? m.replace(/\.?0+$/, "") : m
	if (e < -4 || e >= 6) {
		const [m, p] = x.toExponential(5).split("e")
		return `${trim(m)}e${p[0] == "-" ? "-" : "+"}${p.replace(/^[+-]/, "").padStart(2, "0")}`
	}
	return trim(x.toFixed(5 - e))
}

/// A value thrown and not caught, a box, as text, as bober_uncaught shows it.
const shown = box => {
	const [kind, bits] = [ex.boxTag(box), ex.boxBits(box)]
	const f = new DataView(new ArrayBuffer(8))
	f.setBigInt64(0, bits, true)
	switch (kind) {
		case 1: return "null"
		case 2: return bits & 1n ? "true" : "false"
		case 3: case 4: case 5: case 6: return `${BigInt.asIntN(64, bits)}`
		case 7: case 8: case 9: case 10: return `${BigInt.asUintN(64, bits)}`
		case 11: return g(f.getFloat32(0, true))
		case 12: return g(f.getFloat64(0, true))
		case 13: return str(ex.boxRef(box))
		default: return "<value>"
	}
}

/// What e, which code threw, prints: a trap's message, or of a throw not caught.
const failure = e => {
	if (e instanceof Trap) return e.message
	if (e instanceof WebAssembly.Exception && e.is(ex.bober)) {
		const box = e.getArg(ex.bober, 0)
		return trapped(`uncaught throw: ${shown(box)}`, ...sites.get(box))
	}
	// Of the engine: a stack overflow, else what checks bober's code does not make, as of a null.
	if (e instanceof RangeError && /call stack/.test(e.message)) return trapped("stack overflow", null, -1)
	if (e instanceof WebAssembly.RuntimeError) return trapped(e.message, null, -1)
	throw e
}

// Tests: a report as bober_tested's, of what each printed, captured.
let cap = ""
const all = { up: null, kids: "", failed: false, from: 0 }
let level = all
let sent = 0
const esc = s => s.replace(/["\\\x00-\x1f]/g, c => c == '"' || c == "\\" ? `\\${c}` : c == "\n" ? "\\n" : c == "\t" ? "\\t"
	: `\\u${c.charCodeAt(0).toString(16).padStart(4, "0")}`)
const key = (l, name) => { l.kids += `${l.kids ? ',"' : '"'}${esc(name)}":` }
const flush = () => { write(1, all.kids.slice(sent)); sent = all.kids.length }

/// Ends l, which failed with msg if not null.
const end = (l, msg) => {
	const up = l.up
	if (msg != null) {
		const to = l.failed ? (key(l, "trap"), l) : up
		let b = cap.slice(l.from)
		if (b.length > 4096) {
			b = b.slice(-4096)
			while (b && (b.charCodeAt(0) & 0xc0) == 0x80) b = b.slice(1)
		}
		to.kids += `"${esc(b)}${b && !b.endsWith("\n") ? "\\n" : ""}${esc(msg)}"`
	}
	if (l.failed) up.kids += `{${l.kids}}`
	else if (msg == null) up.kids += '"ok"'
	up.failed ||= l.failed || msg != null
}

/// Runs f, which gives the promise of a call into the module, as the test or subtest name, in the running one if any;
/// it fails if the call or a fiber traps.
const run = async (name, f) => {
	key(level, name)
	flush()
	const l = { up: level, kids: "", failed: false, from: cap.length }
	level = l
	depth++
	let msg = null
	try {
		await Promise.race([new Promise((_, no) => { l.fail = no }), f()])
	} catch (e) {
		msg = failure(e)
		reset()
	}
	depth--
	end(l, msg)
	level = l.up
	flush()
}

// Fibers, as src/fiber.c's: each a promising call into the module, one running at a time until it parks by a
// suspending import, whose promise resolves once it is the next ready, or ends; then the next ready runs. The first
// runs start, and each test and subtest as a call of its own.
let fibers, running, ready
let made = 0n
let call // the module's call of a closure () => null, which gives a promise
const suspending = f => new WebAssembly.Suspending(f)

/// A promise that resolves once the running fiber, which parks, runs again.
const resumed = () => new Promise(r => { running.resume = r })

/// Parks the running fiber: a promise that resolves once it runs again.
const park = () => {
	const p = resumed()
	next()
	return p
}

/// Readies fiber f, unless it is already or was dropped.
const wake = f => { if (f && !f.queued) f.queued = true, ready.push(f) }

// Sleepers, as src/io.c's: in a heap by deadline, the soonest first, which wakes those of one deadline in its order. In
// tests the time is logical, as bober_mono's: it waits for nothing, but moves to the soonest deadline once no fiber is
// ready.
const MAX = 2n ** 63n - 1n // a deadline that never comes
let heap
let logical = tests ? 1n : -1n
const mono = () => logical >= 0n ? logical : process.hrtime.bigint()

/// Puts w at i in the heap and moves it up or down to where its deadline belongs.
const sift = (w, i) => {
	for (let up; i && heap[up = (i - 1) >> 1].deadline > w.deadline; i = up) heap[i] = heap[up]
	for (let c; (c = 2 * i + 1) < heap.length; i = c) {
		if (c + 1 < heap.length && heap[c + 1].deadline < heap[c].deadline) c++
		if (heap[c].deadline >= w.deadline) break
		heap[i] = heap[c]
	}
	heap[i] = w
}

/// Wakes the sleepers whose deadline passed.
const expire = () => {
	for (const t = mono(); heap.length && heap[0].deadline <= t;) {
		const w = heap[0]
		const last = heap.pop()
		if (heap.length) sift(last, 0)
		w.done = true
		wake(w.fiber)
	}
}

/// Runs the next fiber ready, once a sleeper's deadline passes if none is.
const next = () => {
	if (!ready.length && heap.length && heap[0].deadline < MAX) {
		if (logical < 0n) {
			const ms = Number((heap[0].deadline - mono()) / 1000000n) + 1
			return void setTimeout(() => (expire(), next()), Math.min(ms, 2 ** 31 - 1))
		}
		logical = heap[0].deadline
		expire()
	}
	const f = ready.shift()
	if (!f) trap("every fiber is parked", null, -1)
	f.queued = false
	running = f
	f.resume()
}

/// Fails the test running, or ends the program, as a fiber threw e.
const fail = e => depth > 0 ? level.fail(e) : (write(2, `${failure(e)}\n`), process.exit(1))

/// Drops every fiber but the first, which runs again, as once a test trapped.
const reset = () => {
	running = { id: 0n }
	fibers = new Map([[0n, running]])
	ready = []
	heap = []
}
reset()

const host = {
	trap: (what, file, at) => trap(str(what), str(file), at),
	thrown: (box, file, at) => { sites.set(box, [str(file), at]) },
	test: suspending(async (name, file, f) => {
		seed = STONEWALL
		testFile = str(file)
		testAt = -1
		if (!all.kids) write(1, "{")
		await run(str(name), WebAssembly.promising(f))
		cap = ""
	}),
	subtest: suspending((name, f) => run(str(name), () => call(f))),
	mark: at => { testAt = at },
	tested: () => {
		write(1, all.kids ? "}\n" : "{}\n")
		return all.failed ? 1 : 0
	},
	put: (fd, s) => {
		if (tests) cap += str(s)
		else write(fd, str(s))
		return 1
	},
	argc: () => 1 + args.length,
	arg: i => make(i == 0 ? file : args[i - 1] ?? ""),
	now: () => tests ? OBERGEFELL : BigInt(Date.now()) * 1000000n,
	random: () => {
		// SplitMix64.
		let z = seed = BigInt.asUintN(64, seed + 0x9e3779b97f4a7c15n)
		z = BigInt.asUintN(64, (z ^ z >> 30n) * 0xbf58476d1ce4e5b9n)
		z = BigInt.asUintN(64, (z ^ z >> 27n) * 0x94d049bb133111ebn)
		return BigInt.asIntN(64, z ^ z >> 31n)
	},
	readFile: path => {
		try { return make(readFileSync(str(path), "latin1")) } catch { return null }
	},
	writeFile: (path, s) => {
		try { writeFileSync(str(path), Buffer.from(str(s), "latin1")); return 1 } catch { return 0 }
	},
	idOf: (() => {
		const ids = new WeakMap()
		let next = 1n
		return x => typeof x == "object" && x != null ? ids.get(x) ?? (ids.set(x, next), next++) : BigInt(x ?? 0)
	})(),
	bober_fiber: suspending(f => {
		const p = resumed()
		ready.unshift(running)
		running.queued = true
		const id = ++made
		fibers.set(id, running = { id })
		call(f).then(() => (fibers.delete(id), next())).catch(fail)
		return p
	}),
	bober_self: () => running.id,
	bober_park: suspending(park),
	bober_wake: id => wake(fibers.get(id)),
	bober_mono: mono,
	bober_io_sleep: suspending(async ns => {
		if (ns <= 0n) return
		const t = mono()
		const w = { fiber: running, deadline: ns > MAX - t ? MAX : t + ns }
		heap.push(w)
		sift(w, heap.length - 1)
		while (!w.done) await park()
	}),
	fmod: (x, y) => x % y,
	pow: Math.pow,
}

const imports = new Proxy(host, { get: (h, name) => h[name] ?? (() => trap(`${String(name)}: not on WebAssembly`, null, -1)) })
const { instance } = await WebAssembly.instantiate(readFileSync(file), { bober: imports })
ex = instance.exports
call = WebAssembly.promising(ex.call)
WebAssembly.promising(ex.start)().then(r => { process.exitCode = r }, fail)
