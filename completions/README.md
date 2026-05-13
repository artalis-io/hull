# Hull shell completions

Tab completion for `hull` in bash, zsh, and fish.

## Bash

```sh
# System-wide:
sudo cp hull.bash /etc/bash_completion.d/hull

# User-local (no sudo):
mkdir -p ~/.local/share/bash-completion/completions
cp hull.bash ~/.local/share/bash-completion/completions/hull
```

Then restart your shell, or `source` the file to enable it in the current session.

Requires the `bash-completion` package (most distros include it by default; on macOS: `brew install bash-completion@2`).

## Zsh

```sh
# User-local:
mkdir -p ~/.zsh/completions
cp _hull ~/.zsh/completions/_hull
```

Then add to `~/.zshrc`:

```sh
fpath=(~/.zsh/completions $fpath)
autoload -Uz compinit && compinit
```

If you use Homebrew on macOS, you can drop it into the system path:

```sh
cp _hull "$(brew --prefix)/share/zsh/site-functions/_hull"
```

(May require `rm -f ~/.zcompdump && compinit` to pick up.)

## Fish

```sh
mkdir -p ~/.config/fish/completions
cp hull.fish ~/.config/fish/completions/
```

Completions take effect immediately on a new shell.

## What's completed

* All 19 subcommands with descriptions
* `hull build --compiler=` → `tcc`, `system`, `cc`, `gcc`, `clang`, `cosmocc`
* `hull new` / `hull init --runtime=` → `lua`, `js`
* `hull agent <sub>` → `routes`, `db`, `request`, `status`, `errors`, `test`, `context`, `migrate`, `deploy`
* `hull agent db <sub>` → `schema`, `query`
* `hull agent request <method>` → `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, `HEAD`, `OPTIONS`
* `hull agent context --task=` → `auth`, `db`, `routing`, `middleware`, `testing`, …
* `hull migrate <sub>` → `status`, `new`
* `hull deploy <target>` → `dockerfile`, `systemd`, `fly`
* `hull doctor`, `hull version` → `--json`
* `hull dev` → `-p`, `-d`, `--agent`, `--no-migrate`, `--no-sandbox`, `--audit`
* `hull build` → `-o`, `--cc`, `--compiler`, `--sign`, `--developer-key`, `--target`, `--no-aot`
