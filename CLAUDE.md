i want to build my own language focused on the build-test loop speed.
idea is a ts+go+zig merger. ideas of ts, speed and compiledness of go/zig with incremental compilation and package manager, formatter, linter everything built in.

For this project, me reading the code is the expensive part. so keep changes small and elegant. before finishing, let another agent review your changes and give feedback if the direction how you implemented this is right, wether there are options to trim and simplify.

When commiting, no Co-Authored-By. Keep commit messages short, like the other commits.

Performance of the boot interpreter does not matter. The boot interpreter only has to be able to bootstrap the actual native compiler. Always use native bober and only check at the end that the boot interpreter can still bootstrap the native bober.

Git: main is what I have reviewed, dev is not yet reviewed. Commit directly to dev and fold each change into the existing commit it belongs to (amend, or rebase and squash, then force push dev), so every commit stays one small logical change set and no fixup commits remain.
