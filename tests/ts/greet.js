export const HELLO = "héllo";
export function greet(p, punct) { return `${p.title === undefined ? "" : p.title + " "}${p.name} (${p.age})${punct ?? "!"}`; }
export function feel(m) { return m === 2 ? "sad" : "glad"; }
export async function later(ms, value) { await new Promise(r => setTimeout(r, ms)); return value === "" ? undefined : value; }
export function people(names) { return names.map((name, i) => ({ name, age: i })); }
let said = "";
export function shout(s) { said = s.toUpperCase(); }
export function last() { return said === "" ? undefined : said; }
