# AIN 2.22 DOSBox archives

Reference archives created with the historical **AIN 2.22** DOS binary
(`http://old-dos.ru/dl.php?id=15243`, file `AIN.ver.2.22.English.zip`)
under DOSBox 0.74, then decoded by the native C port (`build/ain`).

## Layout

- `historical_dos/ain222/` — original 2.22 distribution (`AIN.EXE`, `AINEXT.EXE`, `AINEXE.EXE`, `AIN.DOC`)
- `packed_corpus/ain222/` — single-volume archives produced by `AIN.EXE 2.22`
- `packed_corpus/ain222/volumes/` — `/F` fragmented archives (`.AIN` + `.001` `.002` ...)
- `tests/verify_ain222.sh` — CRC + byte-identical extract check against `corpus/`

## Recreate under DOSBox

Copy `historical_dos/ain222/AIN.EXE` and 8.3 corpus files into a DOS
directory, copy `tests/ain222/dosbox/MAKEALL.BAT` (and `MAKEVOL.BAT`)
there, then run DOSBox with `tests/ain222/dosbox/dosbox.conf`
(AIN 2.22 is WWPACK-packed; send Enter if it waits on a splash).

```
AIN A /M1 /Y HELLO1.AIN HELLO.TXT
AIN A /M2 /Y HELLO2.AIN HELLO.TXT
AIN A /M3 /Y HELLO3.AIN HELLO.TXT
AIN A /M4 /Y HELLO4.AIN HELLO.TXT
AIN A /M1 /R /Y NESTED.AIN NESTED\*.*
AIN A /F20K /M3 /Y VOL20.AIN ENG.TXT MIXED.BIN IMAGE.RAW HELLO.TXT CODE.C
```

## Native decode

```
make
./tests/verify_ain222.sh
```

AIN 2.22 `/F` volumes each carry a complete index (`idx_pos != 0`) and may
contain only a slice of a split file. The C port treats those volumes as
standalone archives (unlike AIN 2.32, which uses a single solid stream
sliced across `.A01` fragments with `idx_pos = 0` on intermediate volumes).
