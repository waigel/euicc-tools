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
 * The line a member is declared on, or null.
 *
 * A text search and not a parse: it finds the type's assignment and then the
 * first line inside it that begins with the member's name. The schema itself
 * comes from asn1c's descriptors and never from here, so a miss costs a link
 * and nothing else -- which is why a search is enough and a second ASN.1
 * parser would be too much.
 */
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

export function declarationLine(
  asn: string,
  type: string,
  member: string
): number | null {
  const lines = asn.split("\n");
  /* An inline type is declared inside the assignment of the type it sits in,
     and only that assignment has a name to search for. */
  const outer = type.split("__")[0];
  const head = new RegExp(`^${outer.replace(/[-[\]{}()*+?.\\^$|]/g, "\\$&")}\\s*::=`);
  const field = new RegExp(`^\\s*${member.replace(/[-[\]{}()*+?.\\^$|]/g, "\\$&")}\\s`);
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
  const partial = text.slice(start, offset);

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
  while (i < start) {
    const c = text[i];

    /* X.680 12.6.3: a line comment ends at a newline or at a second --. */
    if (c === "-" && text[i + 1] === "-") {
      i += 2;
      while (i < start && text[i] !== "\n") {
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
      while (i < start && !(text[i] === "*" && text[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    if (c === '"') {
      i++;
      while (i < start && text[i] !== '"') i++;
      i++;
      continue;
    }
    if (c === "'") {
      i++;
      while (i < start && text[i] !== "'") i++;
      i++;
      if (i < start && /[HhBb]/.test(text[i])) i++;
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
      while (j < start && WORD.test(text[j])) j++;
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
      if (expect === null) {
        const sc = nameScope(schema, top().type);
        const m = sc?.type.members?.find((x) => x.name === word);
        top().used.push(word);
        labels.push(word);
        owner = sc?.owner ?? null;
        expect = m ?? { name: word, type: "", optional: false };
      } else if (schema.types[expect.type]?.kind === "CHOICE"
                 || schema.types[expect.type]?.kind === "OPEN TYPE") {
        const alt: Member | undefined =
          schema.types[expect.type].members?.find((x) => x.name === word);
        labels.push(word);
        owner = expect.type;
        expect = alt ?? { name: word, type: "", optional: false };
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
    partial,
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
