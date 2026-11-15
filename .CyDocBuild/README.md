# Documentation authoring

The public manual is `Documentation/Documentation.html`. Distribute its complete
folder. This workspace contains English originals, French translations, copied
license notices and the generation configuration; it is not part of the reader.

Use Node.js 22+ and the official CyDoc source/package recorded in `toolchain.json`.
CyDoc is a documentation dependency, separate from the audio SDKs. No npm registry
package name is assumed to be the official distribution.

From that CyDoc checkout, build and package it using its own documented commands,
then install the resulting archive into this private generation environment:

```sh
npm install --prefix .CyDocBuild/internal/build /absolute/path/to/cydoc-0.1.0.tgz marked@17.0.6 prismjs@1.30.0 esbuild@0.25.12
node .CyDocBuild/build.mjs
node .CyDocBuild/build.mjs --check
```

Use the real archive path. The wrapper checks the recorded generator bytes before
running its CLI. Both commands generate the export; `--check` is not read-only.
CMake neither installs this tooling nor invokes Node/npm or downloads anything.
Only the complete `Documentation/` export is installed with the audio package.

Edit `manual/en` first and then translate every page under `manual/fr`. Preserve
page IDs, explicit heading anchors and parent IDs. Increment `revision` and update
the French `sourceRevision` when the translation catches up. Use `doc:page#anchor`
for internal links. Keep audit reports and working notes outside both content roots.

License texts in `assets/licenses` are byte-exact copies of the project's notices
and the pinned CyDoc/build-dependency licenses; update their source and provenance
together when changing dependencies. They are copied into `DocData/assets/licenses`.

The current generator has a build step that refreshes its own `dist/viewer.css`.
Use the installed package in `internal/build`, not the neighboring CyDoc checkout,
to keep generation writes inside this workspace.
