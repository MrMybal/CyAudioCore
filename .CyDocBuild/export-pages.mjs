import { copyFile, lstat, mkdir, mkdtemp, readdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Export the generated manual only. This command neither commits nor publishes.
if (process.argv.length !== 2) throw new Error('Usage: node .CyDocBuild/export-pages.mjs');
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'Documentation');
const expected = ['DocAI.md', 'DocData', 'Documentation.html'];
const entries = (await readdir(source)).sort();
if (JSON.stringify(entries) !== JSON.stringify(expected)) {
  throw new Error('Unexpected files in Documentation; inspect the export before publishing');
}
const report = JSON.parse(await readFile(join(source, 'DocData/translation-report.json'), 'utf8'));
const manifest = JSON.parse(await readFile(join(source, 'DocData/manifest.json'), 'utf8'));
if (manifest.defaultLocale !== 'en' || report.missing.length || report.outdated.length || report.untracked.length) {
  throw new Error('Regenerate and check the complete English/French manual before exporting');
}
const ids = locale => manifest.pages.filter(page => page.locale === locale).map(page => page.id).sort();
if (!ids('en').length || JSON.stringify(ids('en')) !== JSON.stringify(ids('fr'))) {
  throw new Error('English and French page sets must match');
}
const files = [];
async function collect(relative) {
  const path = join(source, relative);
  const info = await lstat(path);
  if (info.isSymbolicLink() || relative.split(/[\\/]/).some(name => name.startsWith('.') || name === 'AGENTS.md')) {
    throw new Error(`Unexpected publication entry: ${relative}`);
  }
  if (info.isDirectory()) {
    for (const name of (await readdir(path)).sort()) await collect(join(relative, name));
  } else if (info.isFile()) {
    files.push(relative);
  } else {
    throw new Error(`Not a regular manual file: ${relative}`);
  }
}
for (const entry of entries) await collect(entry);
const parent = join(root, 'build/github-pages');
await mkdir(parent, { recursive: true });
const destination = await mkdtemp(join(parent, 'site-'));
for (const relative of files) {
  await mkdir(dirname(join(destination, relative)), { recursive: true });
  await copyFile(join(source, relative), join(destination, relative));
}
// Keep Documentation.html available and serve the exact same reader at the root.
await copyFile(join(source, 'Documentation.html'), join(destination, 'index.html'));
await writeFile(join(destination, '.nojekyll'), '');
console.log(JSON.stringify({ directory: destination, files: files.length + 2 }, null, 2));
