# Happy Wheels pour NumWorks (.nwa)

Portage de **Happy Wheels** (version mobile 1.1.2, celle de ton archive Vita) sur
calculatrice **NumWorks**, jouable avec les touches de la calculatrice.

**Crédits :**
- discord.gg/sti2d
- discord : cjnw (yamii)
- Jeu original : Fancy Force / Jim Bonacci (Happy Wheels)

Les crédits s'affichent sur l'écran titre, dans les menus de sélection et dans le menu pause.

---

## Installation

1. Branche la calculatrice en USB.
2. Ouvre <https://my.numworks.com/apps> dans Chrome ou Edge, puis envoie `HappyWheels.nwa`.
   Tu peux aussi utiliser la ligne de commande : `npx nwlink install-nwa HappyWheels.nwa`
3. Sur la calculatrice, lance **Happy Wheels** depuis l'écran d'accueil.

Taille installée : **3,7 Mo** (binaire final de 3 889 568 octets), sous la limite de 5 Mo.

## Commandes

| Touche | Action |
|---|---|
| Flèche ▲ / ▼ | Accélérer / freiner, marche arrière |
| Flèche ◀ / ▶ | Se pencher en arrière / en avant |
| **OK** | Action spéciale : saut (Business Guy, Shopper), jetpack (Wheelchair Guy), frein (Dad), saut chargé (Pogo : maintenir ▲ puis relâcher) |
| **⌫** (Backspace) | S'éjecter du véhicule |
| **EXE** | Recommencer le niveau |
| **Back** | Pause (Reprendre / Recommencer / Choix du niveau / Menu principal) |
| **Back** sur l'écran titre, ou **Home** | Quitter |
| **+ / −** | Choix du personnage dans le chapitre « SELECT A CHARACTER » |

## Ce qui est conservé (identique à l'original)

- **Graphismes d'origine** : tous les sprites viennent du jeu (le jeu de sprites « tiny »,
  conçu pour un écran 480×320), réduits à 2/3 pour l'écran 320×240. Le cadrage est donc le même
  que sur un petit téléphone. Fonds parallaxes, logo, menus, chiffres du chronomètre, police
  Clarendon du jeu : tout est d'origine.
- **Les 73 niveaux officiels** (Business Guy, Irresponsible Dad, Wheelchair Guy, Effective
  Shopper, Pogo Stick Guy, et le chapitre « Select a character »), convertis depuis les fichiers
  XML d'origine : formes, couleurs, terrains, groupes, articulations, déclencheurs.
- **Le concept** : ragdoll physique, démembrement, sang, éjection, pièges, ligne d'arrivée,
  chronomètre.
- **Les objets spéciaux** : pics, lames, mines, mines à tête chercheuse, panneaux boost,
  ventilateurs, poutres en I, rondins, plateformes à ressort, boules de démolition, harpons,
  arbalètes, camionnettes, bouteilles, ballons, panneaux, ligne d'arrivée.
- Les paramètres physiques ont été repris du binaire d'origine (limites des articulations du
  ragdoll, filtres de collision, densités des véhicules, structure du Segway…).

## Ce qui a été retiré pour tenir sur la calculatrice

- Sons et musique (la NumWorks n'a pas de haut-parleur) : environ 4 Mo de moins.
- Éditeur de niveaux, niveaux en ligne, publicités, achats intégrés.
- Jeux de sprites haute définition (small/medium/large) : seul le jeu « tiny » est gardé.
- Le personnage Moped Couple (aucun niveau officiel ne l'utilise).
- Les bulles d'aide du tutoriel (prévues pour l'écran tactile) et les textes décoratifs des
  niveaux.
- L'animation d'explosion des mines n'utilise qu'une image sur deux.
- Pas de sauvegarde de progression : les applications NumWorks n'ont pas accès au stockage.
  Tous les niveaux sont donc débloqués d'office.

## À savoir

- Le moteur d'origine (cocos2d-x + Box2D, compilé pour Android) ne peut pas tourner sur la
  calculatrice (256 Ko de RAM, pas de GPU). Le jeu a donc été **réécrit en C** : moteur
  physique, rendu par bandes et logique de jeu. Les données (sprites, niveaux, corps des
  personnages et des véhicules) sont celles du jeu original. La physique reproduit l'original
  au plus près, mais ce n'est pas une copie bit à bit : certains passages scriptés peuvent se
  comporter un peu différemment.
- Le jeu a été testé sur PC dans un simulateur. Les 73 niveaux se chargent et tournent sans
  crash, mais **il n'a pas été essayé sur une vraie calculatrice**.
- RAM : 37 Ko fixes, plus 32 à 104 Ko selon le niveau (médiane 51 Ko). Si un niveau est trop
  gros pour la mémoire de ta calculatrice, le message « Level too big for RAM » s'affiche au
  lieu d'un plantage.
- Si la calculatrice est trop lente, le jeu ajoute des sous-pas physiques pour rester en temps
  réel jusqu'à 20 images/s. En dessous, il ralentit.

## Recompiler

Prérequis : `gcc-arm-none-eabi`, `libnewlib-arm-none-eabi`, Python 3 + Pillow, Node.js (`nwlink`).

```sh
npm install nwlink
make build/HappyWheels.nwa ASSETS="/chemin/vers/Happy Wheels Vita audio fix/data/happywheels/assets"
make sim        # simulateur PC (captures d'écran PPM), ex. : HW_DUMP=30 HW_OUT=shots ./build/sim 0
```

- `tools/build_assets.py` : convertit les sprites et les niveaux d'origine en un bloc binaire unique.
- `src/` : moteur physique (`phys.c`), rendu par bandes (`gfx.c`), niveaux (`level.c`),
  personnages et véhicules (`character.c`), objets spéciaux (`specials.c`), menus et boucle de
  jeu (`game.c`).
