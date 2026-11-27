# AGENTS.md — wup-homebrew (fork wozt, branche `feature/netfs`)

## Projet

Homebrew pour le Wii U GamePad (WUP-010), ARM926EJ-S, 4 Mio de RAM. Trois briques :
- `libwup/` : SDK runtime des apps `.wup` (POSIX, littlefs, sockets lwIP, threads).
- `wup-menu/` : le Homebrew Menu (une app libwup + hook firmware + loader).
- `wup-emu/` : émulateur du GamePad pour tester sans matériel.

Mission en cours : **netfs** — un VFS dans libwup exposant un dossier réseau sous `/net/` via un serveur TCP maison (pas SMB/NFS). Voir `docs/netfs-suivi.md` pour l'avancement.

## Règles dures

- Ne jamais modifier le firmware Nintendo, le hook de boot ou les partitions flash (hors config menu existante 0xB0000/0xB1000).
- Apps et menu ne coexistent pas en mémoire : toute config réseau doit passer par le handoff (0x34F000) ou la flash littlefs, jamais par des globals partagées.
- Pas de dépendances lourdes ; protocole TCP binaire minimal, versionné.
- Serveur : lecture seule d'abord, jamais exposé à Internet, pas de chiffrement (LAN seulement, documenté).

## Build / test sur cette machine (Debian)

```sh
export LLVM_BIN=/usr/lib/llvm-19/bin   # llvm-nm, llvm-objcopy, ld.lld y sont
# ce poste : llvm-ar/llvm-nm/llvm-objcopy non suffixés manquent au PATH ->
mkdir -p /tmp/wupbin && cd /tmp/wupbin && ln -sf /usr/bin/llvm-ar-19 llvm-ar && ln -sf /usr/bin/llvm-nm-19 llvm-nm && ln -sf /usr/bin/llvm-objcopy-19 llvm-objcopy && cd -
export PATH=/tmp/wupbin:$PATH
cd libwup
python3 tools/deps.py                             # fetch littlefs/lwip/picolibc/tinf (une fois)
python3 tools/wupbuild.py examples/hello          # -> build/apps/hello.wup
python3 tools/wupbuild.py <appdir>                # app avec app.cfg (l'app.cfg est lu seulement si c'est le SEUL argument)
```

Menu : `cd ../wup-menu && python3 ../libwup/tools/wupbuild.py menu`

Tests netfs : `cd tools/netfs-server && python3 test_server.py && python3 test_client_proto.py` (12 tests, sans GamePad).

## Conventions du code

- C89-ish embarqué, commentaires denses en anglais (style existant), pas de malloc massif, buffers statiques alignés.
- libwup : `src/sys.c` = POSIX (fichiers = descripteurs 32..39, sockets en dessous), `src/fs.c` = littlefs dans la copie flash inutilisée, `src/net.c` = sockets lwIP, `src/wup.c` = cycle de vie.
- Menu : `menu/ui.c` = écrans LVGL (pile de screens), `menu/main.c` = réseau/config/services, `menu/config.*` = settings flash.
- Protocole netfs : voir `tools/netfs-server/PROTOCOL.md` et `libwup/include/netfs.h`.

## Git

- Branche dédiée `feature/netfs`, commits par étape, descriptifs. Ne rien pousser sur `ravensclawx/wup-homebrew`.

## Installateur WUHB (wup-menu-installer)

Pas de Docker sur ce poste : le devkitPro complet (image docker exportée) est dans `devkitpro/` (gitignoré).

```sh
cd wup-menu && python3 tools/mkinstaller.py ../wup-menu-installer/payload/payload.bin
cd ../wup-menu-installer && make DEVKITPRO=$PWD/../devkitpro
# -> homebrew_menu_installer.wuhb
```

Ne PAS builder l'installateur avec le devkitpro de /opt (il n'a ni les headers mocha ni SDL2_gfx :
un installateur lié à la main donne un écran noir au lancement).
