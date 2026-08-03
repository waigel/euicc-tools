/*
 * What may be written at a point in a file.
 *
 * `euicc schema` prints the schema as a map of types, read from asn1c's own
 * descriptors. This turns that map plus a cursor position into a list of
 * suggestions. It holds no knowledge of ASN.1 beyond the shape of a value: the
 * schema says what a type contains, and nothing here repeats it.
 *
 * The cursor's type is found by scanning the text from the start of the value
 * to the cursor and following the member names through the open braces. That
 * is weaker than what tsserver does for TypeScript, which type-checks the file
 * as you type and knows the type of an expression. It is enough here for a
 * reason that does not hold there: value notation has no inference, no
 * imports and no generics, so the type at a point follows from the path of
 * member names alone and from nothing else.
 *
 * Nothing here validates. A member offered in the wrong place is a wrong
 * suggestion; a wrong file is what `euicc check` reports, and it reports it
 * whether or not the suggestion came from here.
 */

export interface Member {
  name: string;
  type: string;
  optional: boolean;
  /* Identifiers the member accepts in place of a number, where it has any. */
  names?: string[];
  namedBits?: boolean;
}

export interface SchemaType {
  kind: string;
  members?: Member[];
  /* SEQUENCE OF and SET OF: the type of an element. */
  of?: string;
}

export interface Schema {
  root: string;
  types: Record<string, SchemaType>;
  /* The ASN.1 file the schema is written in, for pointing at a declaration. */
  source?: string;
}

/*
 * An inline type has no name in the ASN.1; `euicc schema` gives it the key the
 * annotation table would use, Parent__member. A reader has never seen that.
 *
 *   File__Member                             File
 *   ProfileHeader__eUICC-Mandatory-AIDs      ProfileHeader.eUICC-Mandatory-AIDs
 *
 * The first is the element type of a list, and its alternatives are written
 * inside the assignment of File itself, so File is where they are declared and
 * what a reader wrote.
 */
export function readableType(name: string): string {
  const i = name.indexOf("__");
  if (i < 0) return name;
  const rest = name.slice(i + 2);
  return rest === "Member" ? name.slice(0, i) : `${name.slice(0, i)}.${rest}`;
}

const esc = (s: string) => s.replace(/[-[\]{}()*+?.\\^$|]/g, "\\$&");

/*
 * The line that assigns a type: `File ::= SEQUENCE OF CHOICE {`. An inline
 * type has no assignment of its own, so it lands on the one that holds it,
 * which is the nearest thing the ASN.1 has to a definition of it.
 */
export function assignmentLine(asn: string, type: string): number | null {
  const head = new RegExp(`^${esc(type.split("__")[0])}\\s*::=`);
  const lines = asn.split("\n");
  for (let i = 0; i < lines.length; i++) if (head.test(lines[i])) return i;
  return null;
}

/*
 * The line a member is declared on, or null.
 *
 * A text search and not a parse: it finds the type's assignment and then the
 * first line inside it that begins with the member's name. The schema itself
 * comes from asn1c's descriptors and never from here, so a miss costs a link
 * and nothing else -- which is why a search is enough and a second ASN.1
 * parser would be too much.
 */
export function declarationLine(
  asn: string,
  type: string,
  member: string
): number | null {
  const lines = asn.split("\n");
  /* An inline type is declared inside the assignment of the type it sits in,
     and only that assignment has a name to search for. */
  const head = new RegExp(`^${esc(type.split("__")[0])}\\s*::=`);
  const field = new RegExp(`^\\s*${esc(member)}\\s`);
  let inside = false;
  for (let i = 0; i < lines.length; i++) {
    if (!inside) {
      if (head.test(lines[i])) inside = true;
      continue;
    }
    if (field.test(lines[i])) return i;
    if (/^\}/.test(lines[i])) return null;
  }
  return null;
}

export interface Suggestion {
  label: string;
  insert: string;
  snippet?: boolean;
  detail?: string;
  doc?: string;
  /* Mandatory members sort above optional ones. */
  sort: string;
  value?: boolean;
}

interface Frame {
  /* null where the schema has no entry, an OBJECT IDENTIFIER for one. */
  type: string | null;
  used: string[];
  /* The names this brace was opened for. Usually one, but a CHOICE selection
     adds a second without a brace of its own: `algoConfiguration
     algoParameter : { … }` is two steps into the schema and one brace. */
  labels: string[];
}

/*
 * Where a member name goes at a point, and which type declares it.
 *
 * Usually the type the braces hold. Not for a list whose element is a CHOICE:
 * a CHOICE value is written `alt : value` and carries no braces of its own, so
 * `File ::= SEQUENCE OF CHOICE { … }` has its alternatives written straight
 * inside the list, and euicc rejects an element wrapped in braces with
 * "expected an alternative name for CHOICE". The names there belong to the
 * element type, not to the list.
 */
export function nameScope(
  schema: Schema,
  typeName: string | null
): { owner: string; type: SchemaType } | null {
  const t = typeName ? schema.types[typeName] : undefined;
  if (!t || !typeName) return null;
  if ((t.kind === "SEQUENCE OF" || t.kind === "SET OF") && t.of) {
    const el = schema.types[t.of];
    if (el && (el.kind === "CHOICE" || el.kind === "OPEN TYPE"))
      return { owner: t.of, type: el };
  }
  return { owner: typeName, type: t };
}

export interface Context {
  /* The type whose members the braces around the cursor hold. */
  type: string | null;
  /* The type that declares `expect`, which is not always `type`. */
  owner: string | null;
  path: string[];
  used: string[];
  /*
   * The member whose value comes next, once its name has been written. Null
   * where a member name is what goes here.
   */
  expect: Member | null;
  partial: string;
}

const WORD = /[A-Za-z0-9-]/;

/*
 * Where the cursor is. The scan runs to the start of the word being typed and
 * not to the cursor: the half-written word is what is being completed and
 * counting it as a member already present would hide it from its own list.
 */
export function analyze(schema: Schema, text: string, offset: number): Context {
  let start = offset;
  while (start > 0 && WORD.test(text[start - 1])) start--;
  return { ...scan(schema, text, start), partial: text.slice(start, offset) };
}

/*
 * The file laid out: one member to a line, indented by depth.
 *
 * Two things it deliberately does not do.
 *
 * It does not go through `euicc show`. That writes canonical value notation and
 * looks like a formatter until you notice it re-serialises a decoded value:
 * every comment is gone and `myHeader ProfileElement ::=` comes back `value1`.
 * Format on save would delete documentation without a word.
 *
 * It does not move anything but whitespace. The guarantee is that the text with
 * every run of whitespace collapsed is unchanged, which is checked rather than
 * assumed, so no comment, name or value can be lost however the layout moves.
 *
 * A brace or a comma inside a comment or a string is not one, which is why this
 * skips them the way the walk above does. A quote is tracked across lines: the
 * writer wraps a long hstring, X.680 12.12 ignores the whitespace inside one,
 * and without that state the closing quote reads as an opening one and every
 * brace after it on the line is lost.
 */
/* Past the spaces and at most one newline after a break we made ourselves, so
   the newline that was already there does not become a blank line. */
function swallow(text: string, i: number): number {
  while (text[i] === " " || text[i] === "\t") i++;
  if (text[i] === "\r") i++;
  if (text[i] === "\n") {
    i++;
    while (text[i] === " " || text[i] === "\t") i++;
  }
  return i;
}

export function layout(text: string, unit = "    "): string {
  const out: string[] = [];
  let line = "";
  let depth = 0;
  let pending = 0; /* depth this line is written at, fixed when it starts */
  let inBlock = false;
  let inQuote: '"' | "'" | null = null;
  let verbatim = false; /* a continuation line of a comment or an hstring */

  const flush = () => {
    const body = line.trim();
    /*
     * A continuation line of a comment or a literal is the author's, so it goes
     * out as written -- and inside a cstring not even the trailing whitespace
     * may go, because every character of one counts. Stripping it shortened the
     * string and the token check could not see it, which is the whole reason
     * that check exists.
     */
    if (verbatim) out.push(inQuote === '"' ? line : line.replace(/\s+$/, ""));
    else out.push(body ? unit.repeat(Math.max(0, pending)) + body : "");
    line = "";
    verbatim = inBlock || inQuote !== null;
    pending = depth;
  };

  for (let i = 0; i < text.length; i++) {
    const c = text[i];

    /*
     * A cstring may run over lines and every character of one counts, so the
     * whole literal stays in one accumulated line and goes out as written. The
     * newline is handled before the quote state below, which is why the guard
     * has to be here: flushing at it trimmed the trailing whitespace off the
     * first line of the string.
     */
    if (c === "\n") {
      if (inQuote === '"') { line += c; continue; }
      flush();
      continue;
    }

    if (inBlock) {
      line += c;
      if (c === "/" && text[i - 1] === "*") inBlock = false;
      continue;
    }
    if (inQuote) {
      line += c;
      if (c === inQuote) inQuote = null;
      continue;
    }
    if (c === "-" && text[i + 1] === "-") {
      /* A line comment runs to the newline; take it whole. */
      while (i < text.length && text[i] !== "\n") line += text[i++];
      i--;
      continue;
    }
    if (c === "/" && text[i + 1] === "*") { inBlock = true; line += "/*"; i++; continue; }
    if (c === '"' || c === "'") { inQuote = c as '"' | "'"; line += c; continue; }

    if (c === "}" || c === "]") {
      /* The brace closes something, so it belongs at that level, and it starts
         its own line unless the line so far is only whitespace. */
      if (line.trim()) flush();
      depth = Math.max(0, depth - 1);
      pending = depth;
      line += c;
      continue;
    }
    if (c === "{" || c === "[") {
      /*
       * An OBJECT IDENTIFIER is a brace list of arcs and the writer keeps it on
       * one line: `{ 2 23 143 1 2 1 }`. Breaking it would put six lines where
       * the writer puts one, and the writer's output has to come back
       * unchanged. Numbers only is what tells such a group from a value.
       */
      const close = text.indexOf(c === "{" ? "}" : "]", i + 1);
      if (close > i && /^[0-9\s]*$/.test(text.slice(i + 1, close))) {
        const inner = text.slice(i + 1, close).trim();
        line += inner ? `${c} ${inner} ${text[close]}` : `${c} ${text[close]}`;
        i = close;
        continue;
      }
      line += c;
      depth++;
      flush();
      i = swallow(text, i + 1) - 1;
      continue;
    }
    if (c === ",") {
      line += c;
      /* A comment after the comma stays on the line it comments on. */
      let j = i + 1;
      while (j < text.length && (text[j] === " " || text[j] === "\t")) j++;
      const trailing = text[j] === "-" && text[j + 1] === "-";
      if (!trailing) { flush(); i = swallow(text, j) - 1; }
      continue;
    }
    line += c;
  }
  flush();

  /* One trailing newline, and no run of blank lines longer than one. */
  const joined = out.join("\n").replace(/\n{3,}/g, "\n\n").replace(/\s+$/, "");
  return joined + "\n";
}

/*
 * The tokens of a file, which layout may not change.
 *
 * Comparing the text with whitespace collapsed does not work, because inserting
 * a line break where there was nothing at all adds whitespace that was not
 * there -- `},ef-dir` becomes `}, ef-dir`. Comparing it with whitespace removed
 * is too weak the other way: it would not notice `major-version 2` becoming
 * `major-version2`. The token list is what a formatter must preserve exactly,
 * so that is what is compared.
 */
export function tokens(text: string): string[] {
  const out: string[] = [];
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (/\s/.test(c)) continue;
    if (c === "-" && text[i + 1] === "-") {
      let j = i;
      while (j < text.length && text[j] !== "\n") j++;
      out.push(text.slice(i, j).trimEnd());
      i = j - 1;
      continue;
    }
    if (c === "/" && text[i + 1] === "*") {
      const end = text.indexOf("*/", i + 2);
      const j = end < 0 ? text.length : end + 2;
      out.push(text.slice(i, j));
      i = j - 1;
      continue;
    }
    if (c === '"' || c === "'") {
      const end = text.indexOf(c, i + 1);
      const j = end < 0 ? text.length : end + 1;
      const body = text.slice(i, j);
      if (c === "'") {
        /* X.680 12.11 and 12.12: the whitespace inside an hstring or a bstring
           is not part of the value, so wrapping one over lines is free. */
        out.push(body.replace(/\s+/g, "") + (text[j] ?? ""));
        i = /[HhBb]/.test(text[j] ?? "") ? j : j - 1;
      } else {
        /* A cstring is text. Every character in it counts, and comparing it
           with the whitespace stripped made this check blind to a formatter
           that shortened one. */
        out.push(body);
        i = j - 1;
      }
      continue;
    }
    if (WORD.test(c)) {
      let j = i;
      while (j < text.length && WORD.test(text[j])) j++;
      out.push(text.slice(i, j));
      i = j - 1;
      continue;
    }
    out.push(c);
  }
  return out;
}

/* What an identifier turned out to be. */
export interface Classified {
  offset: number;
  length: number;
  kind: "member" | "alternative" | "value" | "type";
}

/*
 * Every identifier in the file, and what the schema makes of it.
 *
 * The same walk completion uses, run to the end instead of to the cursor. A
 * grammar has to guess some of this from punctuation -- a colon says
 * alternative, a line start says member -- and guesses wrong where the
 * punctuation is missing or the word is a value standing alone. Here the
 * schema decides, and the editor is told rather than shown.
 */
export function classify(schema: Schema, text: string): Classified[] {
  const out: Classified[] = [];
  scan(schema, text, text.length, (c) => out.push(c));
  return out;
}

function scan(
  schema: Schema,
  text: string,
  stop: number,
  emit?: (c: Classified) => void
): Context {
  /*
   * The root frame encloses no braces: a file is a list of ProfileElement
   * values written directly, so what is expected there is one of those and
   * the frame itself has no members.
   */
  const rootMember: Member = { name: "", type: schema.root, optional: false };
  let stack: Frame[] = [{ type: null, used: [], labels: [] }];
  let expect: Member | null = rootMember;
  /* The type that declares `expect`; null while the root value is expected. */
  let owner: string | null = null;
  let labels: string[] = [];
  const top = () => stack[stack.length - 1];

  let i = 0;
  while (i < stop) {
    const c = text[i];

    /* X.680 12.6.3: a line comment ends at a newline or at a second --. */
    if (c === "-" && text[i + 1] === "-") {
      i += 2;
      while (i < stop && text[i] !== "\n") {
        if (text[i] === "-" && text[i + 1] === "-") {
          i += 2;
          break;
        }
        i++;
      }
      continue;
    }
    if (c === "/" && text[i + 1] === "*") {
      i += 2;
      while (i < stop && !(text[i] === "*" && text[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === '"') {
      i++;
      while (i < stop && text[i] !== '"') i++;
      i++;
      continue;
    }
    if (c === "'") {
      i++;
      while (i < stop && text[i] !== "'") i++;
      i++;
      if (i < stop && /[HhBb]/.test(text[i])) i++;
      continue;
    }

    /* A second value in the same file starts over at the root. */
    if (c === ":" && text[i + 1] === ":" && text[i + 2] === "=") {
      stack = [{ type: null, used: [], labels: [] }];
      expect = rootMember;
      owner = null;
      labels = [];
      i += 3;
      continue;
    }

    if (c === "{") {
      const f = top();
      const t = f.type ? schema.types[f.type] : undefined;
      let next: string | null = null;
      if (expect) {
        next = expect.type;
      } else if (t && (t.kind === "SEQUENCE OF" || t.kind === "SET OF")) {
        /*
         * An element of a list carries no member name of its own. A CHOICE
         * element does not reach here, because it has no braces either and
         * nameScope resolved its alternative before this point.
         */
        next = t.of ?? null;
      }
      stack.push({
        type: next && schema.types[next] ? next : null,
        used: [],
        labels,
      });
      expect = null;
      labels = [];
      i++;
      continue;
    }
    if (c === "}") {
      if (stack.length > 1) stack.pop();
      expect = null;
      labels = [];
      i++;
      continue;
    }
    if (c === ",") {
      expect = null;
      labels = [];
      i++;
      continue;
    }

    if (/[A-Za-z]/.test(c)) {
      let j = i;
      while (j < stop && WORD.test(text[j])) j++;
      const word = text.slice(i, j);
      /*
       * Three things an identifier can be, and which one follows from what is
       * expected here rather than from the word:
       *
       *   nothing expected      a member of the type these braces hold
       *   a CHOICE expected     one of its alternatives, written `name :`
       *                         with no brace of its own
       *   anything else         part of the value, a NULL or an identifier
       *                         standing for a number, and not a name
       */
      const here = (kind: Classified["kind"]) =>
        emit?.({ offset: i, length: word.length, kind });

      if (expect === null) {
        const sc = nameScope(schema, top().type);
        const m = sc?.type.members?.find((x) => x.name === word);
        top().used.push(word);
        labels.push(word);
        owner = sc?.owner ?? null;
        /*
         * A member of a CHOICE is an alternative, and the two are written
         * differently, so they are not the same thing to a reader either.
         */
        if (m) here(sc?.type.kind === "CHOICE" || sc?.type.kind === "OPEN TYPE"
                    ? "alternative" : "member");
        expect = m ?? { name: word, type: "", optional: false };
      } else if (schema.types[expect.type]?.kind === "CHOICE"
                 || schema.types[expect.type]?.kind === "OPEN TYPE") {
        const alt: Member | undefined =
          schema.types[expect.type].members?.find((x) => x.name === word);
        labels.push(word);
        owner = expect.type;
        if (alt) here("alternative");
        expect = alt ?? { name: word, type: "", optional: false };
      } else if (expect.names?.includes(word)) {
        /* An identifier standing for a number, which is a value and not a
           name -- the case a grammar cannot tell from a member at all. */
        here("value");
      } else if (schema.types[word]) {
        /* A type reference, as in `valueN ProfileElement ::=`. */
        here("type");
      }
      i = j;
      continue;
    }
    i++;
  }

  const path: string[] = [];
  for (const f of stack) path.push(...f.labels);
  path.push(...labels);

  return {
    type: top().type,
    owner,
    path,
    used: top().used,
    expect,
    /* analyze fills this in; a full-document scan has no word being typed. */
    partial: "",
  };
}

/*
 * The member the identifier under the cursor names, for a hover.
 *
 * The scan is the same one completion uses, run at the start of the word
 * rather than at the cursor: what a word is follows from what was expected
 * where it begins. Nothing expected there means it names a member of the type
 * the braces hold; a CHOICE expected means it names one of its alternatives.
 */
/* The identifier the cursor is inside, or "". */
export function wordAt(text: string, offset: number): string {
  let a = offset;
  while (a > 0 && WORD.test(text[a - 1])) a--;
  let b = offset;
  while (b < text.length && WORD.test(text[b])) b++;
  return text.slice(a, b);
}

export function memberAt(
  schema: Schema,
  text: string,
  offset: number
): { member: Member; owner: string; path: string[] } | null {
  let start = offset;
  while (start > 0 && WORD.test(text[start - 1])) start--;
  let end = offset;
  while (end < text.length && WORD.test(text[end])) end++;
  const word = text.slice(start, end);
  if (!word || !/^[a-z]/.test(word)) return null;

  const ctx = analyze(schema, text, start);
  /* Past a name, the word is an alternative of what that name expects;
     otherwise it names a member where a member name goes. */
  const sc = ctx.expect
    ? { owner: ctx.expect.type, type: schema.types[ctx.expect.type] }
    : nameScope(schema, ctx.type);
  const member = sc?.type?.members?.find((m) => m.name === word);
  if (!member || !sc) return null;
  return { member, owner: sc.owner, path: [...ctx.path, word] };
}

export function describe(m: Member, owner: string): string {
  const opt = m.optional ? "optional" : "mandatory";
  let doc = `\`${readableType(owner)}.${m.name}\`\n\n${m.type}, ${opt}.`;
  if (m.names?.length) {
    doc += m.namedBits
      ? `\n\nNamed bits: ${m.names.join(", ")}.`
      : `\n\nAccepts: ${m.names.join(", ")}.`;
  }
  return doc;
}

function alternatives(schema: Schema, owner: string): Suggestion[] {
  const t = schema.types[owner];
  /* X.680 29.2: an alternative of a CHOICE is written `name : value`. */
  return (t?.members ?? []).map((m) => ({
    label: m.name,
    insert: `${m.name} : `,
    detail: m.type,
    doc: describe(m, owner),
    sort: `0${m.name}`,
  }));
}

/* What may be written where the cursor is. */
export function suggest(schema: Schema, ctx: Context): Suggestion[] {
  /*
   * A value comes next, because a member name has already been written. What
   * kind of value decides what there is to offer.
   */
  if (ctx.expect) {
    const t = schema.types[ctx.expect.type];
    /* A CHOICE gets no brace of its own; its alternative comes right here. */
    if (t && (t.kind === "CHOICE" || t.kind === "OPEN TYPE"))
      return alternatives(schema, ctx.expect.type);
    /*
     * Otherwise only the identifiers the schema knows are offered. A number
     * or a string is not something to suggest.
     */
    const names = ctx.expect.names;
    if (!names?.length) return [];
    const owner = ctx.expect;
    return names.map((n, i) => ({
      label: n,
      insert: n,
      detail: owner.namedBits ? "named bit" : "named number",
      doc: `A value of \`${owner.name}\`.`,
      sort: String(i).padStart(3, "0"),
      value: true,
    }));
  }

  const sc = nameScope(schema, ctx.type);
  if (!sc) return [];
  const t = sc.type;

  /*
   * A list whose element needs braces of its own. A list of CHOICE does not
   * reach here: nameScope pointed at the element type, and its alternatives
   * are offered below like any other member name.
   */
  if (t.kind === "SEQUENCE OF" || t.kind === "SET OF") {
    if (!t.of || !schema.types[t.of]) return [];
    return [
      {
        label: "{ … }",
        insert: "{ $0 }",
        snippet: true,
        detail: `one ${t.of}`,
        doc: `An element of \`${ctx.type}\`.`,
        sort: "0",
      },
    ];
  }

  if (!t.members) return [];

  const choice = t.kind === "CHOICE" || t.kind === "OPEN TYPE";
  /*
   * nameScope pointed somewhere else, so these braces are a list and each
   * alternative is one element of it. A file is filled in pieces, so
   * fillFileContent belongs in the list as often as the content needs.
   */
  const repeats = sc.owner !== ctx.type;
  return (
    t.members
      /*
       * A CHOICE holds exactly one alternative and a SEQUENCE each member
       * once, so what is written is not offered again. A list is the
       * exception: writing an element does not use anything up.
       */
      .filter((m) => repeats || !ctx.used.includes(m.name))
      .map((m) => ({
        label: m.name,
        /* X.680 29.2: an alternative of a CHOICE is written `name : value`. */
        insert: choice ? `${m.name} : ` : `${m.name} `,
        detail: m.optional ? `${m.type} (optional)` : m.type,
        doc: describe(m, sc.owner),
        sort: `${m.optional ? 1 : 0}${m.name}`,
      }))
  );
}
