export interface Named { name: string }
export interface Person extends Named { title?: string; age: number }
export declare enum Mood { Glad = 1, Sad }
export declare const HELLO = "héllo";
export declare function greet(p: Person, punct?: "!" | "?"): string;
export declare function feel(m: Mood): string;
export declare function later(ms: number, value: string): Promise<string | undefined>;
export declare function people(names: readonly string[]): Person[];
export declare function shout(s: string): void;
export declare function last(): string | undefined;
