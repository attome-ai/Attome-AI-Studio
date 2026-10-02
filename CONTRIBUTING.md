# Contributing to Attome

Thanks for helping. Two rules before you open a pull request.

## 1. License and CLA

Attome is licensed under AGPL-3.0-or-later and also offered under a commercial license ([COMMERCIAL.md](COMMERCIAL.md)). To make that possible, every contributor agrees to the following by adding this line to each commit message (`git commit -s` adds the sign-off; add the CLA line yourself):

```
Signed-off-by: Your Name <you@example.com>
CLA: I agree to the Attome Contributor License Agreement
```

**Attome Contributor License Agreement (summary):**

1. You keep the copyright in your contribution.
2. You grant the project owner (Ahmed Fuad, and any successor or company the owner transfers the project to) a perpetual, worldwide, royalty-free, irrevocable license to use, modify, sublicense and distribute your contribution under **any license**, including the AGPL and commercial licenses.
3. You confirm the contribution is your own work (or you have the right to submit it) and that your employer, if any, does not claim it.
4. Your contribution is provided "as is", without warranty.

Pull requests without the CLA line cannot be merged. This is what lets the project stay free for everyone while companies that cannot use the AGPL pay for a commercial license.

## 2. Dependencies

New dependencies must have a permissive license (MIT, BSD, zlib, Apache-2.0), or LGPL linked dynamically. The CI license gate (ADR-025) enforces this.

## Everything else

Match the surrounding code style, keep changes small, and include a test or bench case for engine changes.
