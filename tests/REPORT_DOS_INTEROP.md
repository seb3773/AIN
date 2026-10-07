# Rapport de test : decode natif des archives AIN DOS 2.22 et 2.2

Date : 2026-10-07
Binaire teste : `build/ain` (portage C, `src/ain.c`)
Emulateur : DOSBox 0.74-3, `cycles=max`, `AIN_SW=/Y`

Verdict : le portage C liste (`l`/`v`), verifie (`t`) et extrait (`x`)
bit-a-bit les archives creees par AIN DOS 2.22 et AIN DOS 2.2.

---

## 1. Binaires DOS sources

| Version reelle (banniere AIN.EXE) | Paquet old-dos.ru | Fichier | AIN.EXE | Editeur |
|---|---|---|---|---|
| **AIN 2.22** | [id=15243](http://old-dos.ru/dl.php?id=15243) | `AIN.ver.2.22.English.zip` | 34340 o, 1995-07-28 | Transas Marine (UK) Ltd. |
| **AIN 2.2** | [id=6990](http://old-dos.ru/dl.php?id=6990) | zip `AIN/` | 35400 o, 1994-01-10 | TOO Infoservice |

Note id=6990 : le `AIN.DOC` du zip titre encore "Версия 2.1" (Infoservice, 1993),
mais la banniere runtime est `AIN 2.2  Copyright (c) 1993  TOO Infoservice`.
Les archives de ce rapport sont donc bien du format 2.2, pas 2.1
(2.0/2.1 etaient documentes comme incompatibles entre eux).

Copies locales :

- `historical_dos/ain222/` — distribution 2.22 anglaise
- `historical_dos/ain22/` — distribution 2.2 russe (id=6990)

Creation sous DOSBox : `AIN A /Mn /Y <archive> <fichiers>` sur le corpus
8.3 (`HELLO.TXT`, `ENG.TXT`, `EMPTY.DAT`, `ZEROS.BIN`, `MIXED.BIN`,
`IMAGE.RAW`, `CODE.C`, arbre `NESTED\`).

---

## 2. Methode de verification

Pour chaque archive :

1. `ain t archive.ain` — CRC flux + index, 0 fichier invalide
2. `ain x archive.ain -o dest/` puis `cmp` octet a octet contre `corpus/`

Scripts reproductibles :

```
./tests/verify_ain222.sh
./tests/verify_ain22.sh
make test
```

---

## 3. AIN DOS 2.22  —  PASS

Corpus : `packed_corpus/ain222/`

Banniere DOS : `AIN 2.22  Copyright (c) 1993-95  Transas Marine (UK) Ltd.`

### 3.1 Archives monocvolume — CRC et extrait identiques

| Archive | Methode | Fichiers | Taille | `ain t` | extrait vs corpus |
|---|---|---|---|---|---|
| HELLO1.AIN | M1 | 1 | 140 | PASS | PASS |
| HELLO2.AIN | M2 | 1 | 140 | PASS | PASS |
| HELLO3.AIN | M3 | 1 | 140 | PASS | PASS |
| HELLO4.AIN | M4 | 1 | 138 | PASS | PASS |
| ENG1.AIN | M1 | 1 | 4221 | PASS | PASS |
| ENG2.AIN | M2 | 1 | 4264 | PASS | PASS |
| ENG3.AIN | M3 | 1 | 5490 | PASS | PASS |
| ENG4.AIN | M4 | 1 | 20428 | PASS | PASS |
| EMPTY1.AIN | M1 | 1 | 85 | PASS | PASS |
| EMPTY3.AIN | M3 | 1 | 85 | PASS | PASS |
| EMPTY4.AIN | M4 | 1 | 67 | PASS | PASS |
| ZEROS3.AIN | M3 | 1 | 972 | PASS | PASS |
| ZEROS4.AIN | M4 | 1 | 200071 | PASS | PASS |
| MIXED1.AIN | M1 | 1 | 5308 | PASS | PASS |
| MIXED2.AIN | M2 | 1 | 5308 | PASS | PASS |
| MIXED3.AIN | M3 | 1 | 5431 | PASS | PASS |
| IMAGE1.AIN | M1 | 1 | 12312 | PASS | PASS |
| IMAGE3.AIN | M3 | 1 | 23820 | PASS | PASS |
| CODE1.AIN | M1 | 1 | 4658 | PASS | PASS |
| MULTI.AIN | M2 | 4 | 15511 | PASS | PASS (4 fichiers) |
| NESTED.AIN | M1 | 7 | 406230 | PASS | PASS (arbre 7 fichiers) |
| FRAG.AIN | M3 | 4 | 35658 | PASS | PASS |

22/22 PASS. DOS `AIN T` sur HELLO1 / MULTI / NESTED : `0 invalid files`.

### 3.2 Fragments `/F` 2.22 — CRC par volume PASS

`packed_corpus/ain222/volumes/` (`AIN A /F20K /M3` et `/F10K /M1`)

Comme 2.32, le flag `0x40` est pose, mais **chaque volume a son propre index**
(`idx_pos != 0`) et l'extension est `.001` / `.002` (pas `.A01`).
Le decodeur C traite alors le volume comme archive autonome.

| Volume | `ain t` |
|---|---|
| VOL10.AIN / .001 / .002 / .003 | PASS |
| VOL20.AIN / .001 / .002 | PASS |

---

## 4. AIN DOS 2.2  —  PASS

Corpus : `packed_corpus/ain22/`

Banniere DOS : `AIN 2.2  Copyright (c) 1993  TOO Infoservice.`

Les flux ne sont **pas** bit-identiques aux archives 2.22 du meme corpus
(tailles legerement differentes : NESTED 407782 vs 406230, ENG1 4223 vs 4221).
Le decode reste correct.

### 4.1 Archives monocvolume — CRC et extrait identiques

| Archive | Methode | Fichiers | Taille | `ain t` | extrait vs corpus |
|---|---|---|---|---|---|
| HELLO1.AIN | M1 | 1 | 140 | PASS | PASS |
| HELLO2.AIN | M2 | 1 | 140 | PASS | PASS |
| HELLO3.AIN | M3 | 1 | 140 | PASS | PASS |
| HELLO4.AIN | M4 | 1 | 139 | PASS | PASS |
| ENG1.AIN | M1 | 1 | 4223 | PASS | PASS |
| ENG2.AIN | M2 | 1 | 4265 | PASS | PASS |
| ENG3.AIN | M3 | 1 | 5490 | PASS | PASS |
| ENG4.AIN | M4 | 1 | 20429 | PASS | PASS |
| EMPTY1.AIN | M1 | 1 | 84 | PASS | PASS |
| EMPTY3.AIN | M3 | 1 | 84 | PASS | PASS |
| EMPTY4.AIN | M4 | 1 | 67 | PASS | PASS |
| ZEROS3.AIN | M3 | 1 | 971 | PASS | PASS |
| ZEROS4.AIN | M4 | 1 | 200071 | PASS | PASS |
| MIXED1.AIN | M1 | 1 | 5308 | PASS | PASS |
| MIXED2.AIN | M2 | 1 | 5308 | PASS | PASS |
| MIXED3.AIN | M3 | 1 | 5431 | PASS | PASS |
| IMAGE1.AIN | M1 | 1 | 12312 | PASS | PASS |
| IMAGE3.AIN | M3 | 1 | 23819 | PASS | PASS |
| CODE1.AIN | M1 | 1 | 4658 | PASS | PASS |
| MULTI.AIN | M2 | 4 | 15481 | PASS | PASS (4 fichiers) |
| NESTED.AIN | M1 | 7 | 407782 | PASS | PASS (arbre 7 fichiers) |

21/21 PASS. DOS `AIN T` sur HELLO1 / MULTI / NESTED : `0 ошибочных файлов`.

### 4.2 Fragments `/F` 2.2 — CRC par volume PASS

`packed_corpus/ain22/volumes/` (`AIN A /F20K /M3`)

Meme layout que 2.22 : flag `0x40` + `idx_pos != 0` sur chaque volume.

| Volume | `ain t` |
|---|---|
| VOL20.AIN | PASS |
| VOL20.001 | PASS |
| VOL20.002 | PASS |

---

## 5. Perimetre et limites

Couvert :

- methodes M1 Ultra, M2 Normal, M3 Fast, M4 Store
- fichier vide, texte, binaire aleatoire/biaise, image, sources C
- archive multi-fichiers solide
- arbre de repertoires (`/R`, chemins `NESTED\DIR1\...`)
- volumes `/F` lus comme archives autonomes (index local)

Hors perimetre de ce rapport :

- recomposition d'un fichier coupe a cheval sur plusieurs volumes 2.2/2.22
  (le format 2.32 solid-slice `.A01` reste le modele multi-volume du portage)
- chiffrement `/G`, SFX AINEXE, commandes d'update DOS

Le corpus 2.32 historique (`packed_corpus/*.AIN`) reste PASS via `make test`.

---

## 6. Conclusion

Le portage C decode correctement :

- les archives **AIN DOS 2.22** (Transas Marine)
- les archives **AIN DOS 2.2** (Infoservice, paquet old-dos.ru id=6990)

CRC OK et extraits identiques au corpus d'origine dans les deux cas.
