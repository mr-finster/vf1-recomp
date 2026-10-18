// Strips every comment from a script for the site: those inside its strings and templates (shaders' GLSL, the page's
// CSS), then the JS ones (terser, which leaves the code itself as it is).
//   node strip.mjs in out   (a .js, or an .html: its <!-- --> comments, inline styles and scripts)
import { readFileSync, writeFileSync } from 'fs';
import { minify } from 'terser';
import { tokenizer } from 'acorn';

// comments inside a string's or template's text: /* */ blocks, // lines, and // after code on a line (only after
// ; { } ) , so a URL's // is left alone)
const inText = s => s
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/^[ \t]*\/\/[^\n]*\n?/gm, '')
  .replace(/^[ \t]*\/\/[^\n]*?(?=\\n)(\\n)?/gm, '')
  .replace(/(^|\\n)[ \t]*\/\/.*?(?=\\n|$)(\\n)?/g, '$1')
  .replace(/([;{}),])[ \t]+\/\/[^\n]*?(?=\\n|\n|$)/g, '$1');

function strings(src) {
  const cuts = [];
  for (const t of tokenizer(src, { ecmaVersion: 'latest', sourceType: 'script', allowHashBang: true })) {
    const k = t.type.label;
    if (k !== 'template' && k !== 'string') continue;
    const raw = k === 'string' ? src.slice(t.start + 1, t.end - 1) : src.slice(t.start, t.end);
    if (!/\/\/|\/\*/.test(raw)) continue;
    const out = inText(raw);
    if (out !== raw) cuts.push([k === 'string' ? t.start + 1 : t.start, k === 'string' ? t.end - 1 : t.end, out]);
  }
  for (let i = cuts.length; i--;) { const [a, b, s] = cuts[i]; src = src.slice(0, a) + s + src.slice(b); }
  return src;
}

async function js(src, keep) {
  const r = await minify(keep ? src : strings(src), { compress: false, mangle: false, format: { comments: false, ascii_only: false } });
  return r.code;
}

const [, , a, b, opt] = process.argv, keep = opt === '--keep-strings';   // (a script whose strings are code it writes, translate.js's: their text left as it is)
let src = readFileSync(a, 'utf8');
if (/\.html$/.test(a)) {
  src = src.replace(/<!--[\s\S]*?-->/g, '');
  src = src.replace(/(<style[^>]*>)([\s\S]*?)(<\/style>)/g, (m, o, c, e) => o + c.replace(/\/\*[\s\S]*?\*\//g, '') + e);
  const parts = []; let last = 0;
  for (const m of src.matchAll(/(<script(?![^>]*\bsrc=)[^>]*>)([\s\S]*?)(<\/script>)/g)) {
    const code = !/\btype=/.test(m[1]) || /\btype=["']?(text\/javascript|module)/.test(m[1]);   // (not a JSON block's)
    parts.push(src.slice(last, m.index), m[1], code && m[2].trim() ? await js(m[2]) : m[2], m[3]); last = m.index + m[0].length;
  }
  parts.push(src.slice(last));
  src = parts.join('');
} else src = await js(src, keep);
writeFileSync(b, src);
