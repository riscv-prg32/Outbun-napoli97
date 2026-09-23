#ifndef OUTBUN_ROUTE_H
#define OUTBUN_ROUTE_H
/*
 * OutBun route: Napoli -> Vietri sul Mare around the Sorrento peninsula.
 *
 * The journey follows the real roads of 1997:
 *   leg 1      SS18 along the Vesuvian coast (Napoli -> Castellammare)
 *   legs 2-4   SS145 "Sorrentina" (Castellammare -> Vico -> Meta -> Sorrento)
 *   leg 5      Via Capo / Massa Lubrense / Termini down to Nerano
 *   leg 6      back over Sant'Agata to Colli di San Pietro, then the SS163
 *   legs 7-9   SS163 "Amalfitana" (Positano -> Amalfi -> Cetara -> Vietri)
 *
 * Each section is `len` * LEN_UNIT road segments of constant curvature with a
 * smooth height change. Curves are signed (positive = bends right), the
 * magnitude follows the real bend: 2 gentle, 4 sweeping, 5-6 hairpin
 * (tornante). The comments name the real places each section reproduces; the
 * sea stays on the right for the whole clockwise tour of the peninsula.
 */
#include <stdint.h>

#define LEN_UNIT 12
#define HILL_UNIT 160

/* section flags */
#define F_TOWN   1   /* built-up: pavements, houses, lamps on both sides     */
#define F_LANDR  2   /* land (not sea) on the right-hand side                */
#define F_TUNNEL 4   /* galleria                                             */
#define F_BRIDGE 8   /* viaduct over a gorge or a marina                     */
#define F_CLIFFL 16  /* limestone cliff face on the left                     */

/* dominant roadside vegetation / landmark */
#define D_NONE 0
#define D_PINE 1
#define D_PALM 2
#define D_LEMON 3
#define D_AGAVE 4
#define D_TOWER 5
#define D_CYPRESS 6
#define D_OLEANDER 7
#define D_BEACH 8    /* umbrellas on the sea side of a town beach           */

typedef struct { uint8_t len; int8_t curve; int8_t hill; uint8_t flags; uint8_t deco; } ob_section_t;

static const ob_section_t ob_sections[] = {
    /* ---- Leg 1: Napoli -> Castellammare di Stabia (SS18, ~30 km) ---- */
    {16, 0, 0, F_TOWN | F_LANDR, D_PALM},      /* Via Marina from Molo Beverello, port cranes    */
    {10, 2, 0, F_TOWN | F_LANDR, D_NONE},      /* bend at Piazza Mercato and the Carmine tower  */
    {14, 0, 1, F_TOWN | F_LANDR, D_NONE},      /* San Giovanni a Teduccio                       */
    {8, -2, 0, F_TOWN, D_PALM},                /* Portici: the Granatello harbour on the right  */
    {12, 1, 2, F_TOWN, D_PALM},                /* Reggia di Portici avenue                      */
    {10, 0, -2, 0, D_PINE},                    /* Ercolano: Miglio d'Oro villas, Vesuvius left  */
    {12, -3, 0, 0, D_AGAVE},                   /* lava-stone coast toward Torre del Greco       */
    {14, 0, 3, F_TOWN, D_NONE},                /* Torre del Greco, coral workshops              */
    {10, 3, -1, 0, D_PINE},                    /* Villa delle Ginestre pinewood                 */
    {16, 0, -2, F_LANDR, D_OLEANDER},          /* Vesuvian vineyards, Lacryma Christi           */
    {10, -2, 1, 0, D_AGAVE},                   /* Torre Annunziata seafront (Oplontis)          */
    {12, 0, 0, F_TOWN, D_NONE},                /* Torre Annunziata                              */
    {14, 2, 0, 0, D_PINE},                     /* Rovigliano rock and the Sarno mouth           */
    {10, -1, 2, F_TOWN | F_LANDR, D_NONE},     /* Castellammare: the naval shipyard             */
    {12, 0, -1, F_TOWN, D_PALM},               /* Villa Comunale di Castellammare               */
    /* ---- Leg 2: Castellammare -> Vico Equense (SS145, ~10 km) ---- */
    {8, 0, 1, F_TOWN, D_OLEANDER},             /* Terme di Stabia, start of the Sorrentina      */
    {10, -3, 2, F_CLIFFL, D_NONE},             /* Pozzano bend, Santa Maria di Pozzano          */
    {12, 0, 0, F_TUNNEL, D_NONE},              /* galleria di Pozzano                           */
    {10, 4, 1, F_CLIFFL, D_PINE},              /* cliff road under Monte Faito                  */
    {8, -4, 0, F_CLIFFL, D_AGAVE},             /* Capo d'Orlando                                */
    {10, 0, 2, F_TUNNEL, D_NONE},              /* galleria Varano                               */
    {12, 3, -1, F_CLIFFL, D_LEMON},            /* Scrajo sulphur springs, lemon terraces        */
    {8, -5, 1, F_CLIFFL, D_NONE},              /* bend above Marina di Vico                     */
    {10, 2, 2, F_TOWN, D_TOWER},               /* Vico Equense, Castello Giusso                 */
    {6, 0, 0, F_TOWN, D_NONE},                 /* Vico centre                                   */
    /* ---- Leg 3: Vico Equense -> Meta (SS145, ~8 km) ---- */
    {8, -2, -1, F_TOWN, D_NONE},               /* leaving Vico                                  */
    {12, 0, 0, F_BRIDGE, D_NONE},              /* Ponte di Seiano, high over Marina di Seiano   */
    {8, 4, 1, F_CLIFFL, D_OLEANDER},           /* Seiano                                        */
    {10, 0, 0, F_TUNNEL, D_NONE},              /* galleria di Seiano                            */
    {10, -5, -1, F_CLIFFL, D_AGAVE},           /* Punta Scutolo, the whole gulf in view         */
    {8, 3, -1, F_CLIFFL, D_PINE},              /* descent toward Alimuri                        */
    {10, 0, 0, F_TUNNEL, D_NONE},              /* galleria di Alimuri                           */
    {10, -2, 0, F_TOWN, D_LEMON},              /* Meta, Basilica del Lauro                      */
    {6, 0, 0, F_TOWN, D_NONE},                 /* Meta centre                                   */
    /* ---- Leg 4: Meta -> Sorrento (SS145, ~5 km) ---- */
    {8, 1, 0, F_TOWN | F_LANDR, D_OLEANDER},   /* Meta - Piano di Sorrento                      */
    {10, 0, 1, F_TOWN | F_LANDR, D_LEMON},     /* Piano di Sorrento, walled citrus groves       */
    {8, -2, 0, F_LANDR, D_LEMON},              /* Sant'Agnello lemon walls                      */
    {10, 2, 0, F_TOWN | F_LANDR, D_PALM},      /* Sant'Agnello, the Cocumella                   */
    {8, 0, 0, F_TOWN, D_PALM},                 /* Corso Italia                                  */
    {8, -1, 0, F_TOWN, D_NONE},                /* Piazza Tasso over the Vallone dei Mulini      */
    /* ---- Leg 5: Sorrento -> Nerano (Massa Lubrense, ~14 km) ---- */
    {8, 2, 1, F_TOWN, D_PALM},                 /* Via Capo, leaving Sorrento westwards          */
    {10, -3, 2, F_CLIFFL, D_PINE},             /* Capo di Sorrento, Bagni della Regina Giovanna */
    {10, 4, 0, 0, D_LEMON},                    /* olive and lemon groves toward Massa           */
    {12, -2, 1, F_TOWN, D_NONE},               /* Massa Lubrense, Capri straight ahead          */
    {10, 5, 3, F_CLIFFL, D_AGAVE},             /* climbing to Termini                           */
    {10, -4, 2, F_LANDR, D_PINE},              /* Termini, Punta Campanella path                */
    {8, 6, -3, F_CLIFFL, D_NONE},              /* tornante                                      */
    {8, -6, -3, F_CLIFFL, D_NONE},             /* tornante                                      */
    {10, 3, -2, 0, D_LEMON},                   /* Nerano terraces                               */
    {8, 0, -1, F_TOWN, D_BEACH},               /* Nerano, Marina del Cantone                    */
    /* ---- Leg 6: Nerano -> Positano (Sant'Agata, SS163, ~20 km) ---- */
    {8, -3, 3, F_CLIFFL, D_NONE},              /* climb out of Nerano                           */
    {10, 5, 3, F_CLIFFL, D_PINE},
    {10, -4, 2, F_LANDR, D_LEMON},             /* Sant'Agata sui Due Golfi                      */
    {8, 0, 1, F_TOWN | F_LANDR, D_CYPRESS},    /* Sant'Agata centre                             */
    {12, 3, 0, F_LANDR, D_PINE},               /* the Nastro Verde ridge                        */
    {8, -2, -1, F_TOWN, D_NONE},               /* Colli di San Pietro: onto the SS163           */
    {10, 5, -2, F_CLIFFL, D_AGAVE},            /* first view of Li Galli                        */
    {8, -6, -2, F_CLIFFL, D_NONE},             /* tornante                                      */
    {10, 4, -2, F_CLIFFL, D_AGAVE},
    {8, -5, -2, F_CLIFFL, D_NONE},             /* tornante above Montepertuso                   */
    {10, 0, 0, F_TUNNEL, D_NONE},              /* galleria                                      */
    {12, 3, -2, F_TOWN, D_NONE},               /* Positano, Chiesa Nuova                        */
    {8, -2, 0, F_TOWN, D_BEACH},               /* Positano, Spiaggia Grande below the Sponda    */
    /* ---- Leg 7: Positano -> Amalfi (SS163, ~17 km) ---- */
    {8, 3, 0, F_TOWN, D_NONE},                 /* leaving Positano                              */
    {12, -4, 1, F_CLIFFL, D_AGAVE},            /* cliff road, Li Galli behind                   */
    {10, 5, 0, F_CLIFFL, D_NONE},              /* Vettica Maggiore                              */
    {8, -3, 0, F_TOWN, D_NONE},                /* Praiano, San Gennaro majolica dome            */
    {10, 4, -1, F_CLIFFL, D_AGAVE},            /* Marina di Praia                               */
    {8, 0, 0, F_BRIDGE, D_NONE},               /* the Fiordo di Furore bridge                   */
    {10, -5, 1, F_CLIFFL, D_NONE},             /* Furore                                        */
    {10, 3, 0, F_TUNNEL, D_NONE},              /* galleria                                      */
    {10, -4, -1, F_CLIFFL, D_TOWER},           /* Capo di Conca Saracen tower                   */
    {8, 2, 0, F_TOWN, D_NONE},                 /* Conca dei Marini, Grotta dello Smeraldo       */
    {10, -3, 0, F_CLIFFL, D_LEMON},            /* Vettica di Amalfi lemons                      */
    {8, 0, 0, F_TOWN, D_NONE},                 /* Amalfi, Duomo di Sant'Andrea                  */
    /* ---- Leg 8: Amalfi -> Cetara (SS163, ~13 km) ---- */
    {6, 2, 0, F_TOWN, D_PALM},                 /* Amalfi seafront                               */
    {8, 0, 0, F_BRIDGE | F_TOWN, D_NONE},      /* the arches over Atrani                        */
    {10, -4, 1, F_CLIFFL, D_NONE},             /* Castiglione                                   */
    {8, 3, 0, F_TOWN, D_BEACH},                /* Minori                                        */
    {8, -3, 0, F_CLIFFL, D_AGAVE},
    {12, 0, 0, F_TOWN, D_BEACH},               /* Maiori, the long seafront                     */
    {10, 5, 2, F_CLIFFL, D_TOWER},             /* Torre Normanna                                */
    {12, -5, 2, F_CLIFFL, D_NONE},             /* Capo d'Orso                                   */
    {8, 4, -1, F_CLIFFL, D_PINE},
    {10, -3, -1, F_CLIFFL, D_TOWER},           /* Erchie and its tower                          */
    {8, 0, -2, F_TOWN, D_NONE},                /* Cetara fishing village                        */
    /* ---- Leg 9: Cetara -> Vietri sul Mare (SS163, ~5 km) ---- */
    {6, 2, 1, F_TOWN, D_NONE},                 /* leaving Cetara harbour                        */
    {10, -4, 1, F_CLIFFL, D_TOWER},            /* Torre di Cetara                               */
    {8, 4, 0, F_CLIFFL, D_AGAVE},              /* Fuenti                                        */
    {10, -3, 0, F_CLIFFL, D_NONE},             /* Albori                                        */
    {8, 3, -1, F_CLIFFL, D_PINE},              /* Marina di Vietri, the Due Fratelli rocks      */
    {10, 0, -1, F_TOWN, D_NONE},               /* Vietri sul Mare, San Giovanni Battista dome   */
};
#define SECTION_COUNT ((int)(sizeof(ob_sections) / sizeof(ob_sections[0])))

/* first section of every leg, plus the end sentinel */
static const uint8_t leg_first_section[10] = {0, 15, 25, 34, 40, 50, 63, 75, 86, 92};

/*
 * Fixed landmarks at their real places, as segments before the end of a leg
 * (the arrival town) with the lateral offset in Q8 road half-widths.
 * Sprite ids are resolved in game.c (LM_SPR_*), keeping this header free of
 * generated asset names.
 */
#define LMS_CRANE 0
#define LMS_TOWER 1
#define LMS_DOME 2
#define LMS_DUOMO 3
#define LMS_DOME_VIETRI 4
#define LMS_STALL 5
typedef struct { uint8_t leg, kind; int16_t before_end, off; } ob_landmark_t;  /* before_end < 0: after the leg start */
static const ob_landmark_t ob_landmarks[] = {
    {0, LMS_CRANE, -75, 900},        /* Napoli: port cranes along Via Marina           */
    {0, LMS_TOWER, -135, -560},      /* Torre del Carmine                              */
    {0, LMS_CRANE, 70, 700},         /* Castellammare naval shipyard gantries         */
    {0, LMS_CRANE, 115, 880},
    {1, LMS_TOWER, 40, -620},        /* Vico Equense, Castello Giusso                  */
    {3, LMS_STALL, 30, -420},        /* limoncello stall on the Sorrento approach      */
    {5, LMS_DOME, 60, 720},          /* Positano, Santa Maria Assunta below the road   */
    {6, LMS_STALL, 70, -420},
    {6, LMS_DUOMO, 26, -640},        /* Amalfi, Duomo di Sant'Andrea                    */
    {8, LMS_DOME_VIETRI, 40, -600},  /* Vietri sul Mare, San Giovanni Battista          */
};
#define LANDMARK_COUNT ((int)(sizeof(ob_landmarks) / sizeof(ob_landmarks[0])))

/* the leg's destination: town names, real distance and the local ingredient */
static const char town_names[10][24] = {
    "NAPOLI", "CASTELLAMMARE DI STABIA", "VICO EQUENSE", "META DI SORRENTO", "SORRENTO",
    "NERANO", "POSITANO", "AMALFI", "CETARA", "VIETRI SUL MARE"};
static const char town_short[10][14] = {
    "NAPOLI", "CASTELLAMMARE", "VICO EQUENSE", "META", "SORRENTO",
    "NERANO", "POSITANO", "AMALFI", "CETARA", "VIETRI"};
static const uint8_t leg_km[9] = {30, 10, 8, 5, 14, 20, 17, 13, 5};
static const char ingredient_names[9][24] = {
    "PANE ROSETTA", "POMODORINO DEL PIENNOLO", "PROVOLONE DEL MONACO", "OLIO EXTRAVERGINE",
    "LIMONE DI SORRENTO", "ZUCCHINE ALLA NERANO", "FIOR DI LATTE AGEROLA", "ALICI DI CETARA",
    "TONNO DI CETARA"};

/* ---- Background: landmark silhouettes (height every 4 px) ---- */
#define LM_VESUVIO 0
#define LM_CAPRI 1
#define LM_ISCHIA 2
#define LM_LIGALLI 3
#define LM_CAMPANELLA 4
#define LM_SALERNO 5
static const uint8_t lm_len[6] = {44, 34, 27, 15, 19, 28};
static const uint8_t lm_heights[6][44] = {
    /* Somma-Vesuvio: the Somma ridge on the left, the Gran Cono on the right */
    {2, 4, 7, 10, 14, 18, 22, 26, 29, 32, 34, 35, 36, 37, 37, 36, 35, 34, 33, 33, 34, 36,
     39, 42, 45, 47, 48, 48, 47, 46, 44, 41, 37, 33, 29, 25, 21, 17, 13, 10, 7, 5, 3, 1},
    /* Capri from the peninsula: Faraglioni, Monte Tiberio, the saddle, Monte Solaro */
    {3, 5, 0, 4, 0, 2, 5, 7, 8, 9, 10, 9, 7, 5, 4, 4, 5, 6, 8, 11, 14, 17, 19, 21, 22, 22, 21,
     19, 16, 12, 8, 5, 3, 1},
    /* Ischia and Monte Epomeo */
    {1, 2, 3, 4, 6, 8, 10, 12, 14, 15, 16, 15, 14, 12, 10, 9, 8, 7, 6, 5, 4, 3, 3, 2, 2, 1, 1},
    /* Li Galli, the Sirens' islets off Positano */
    {0, 2, 3, 2, 0, 0, 3, 4, 4, 3, 0, 0, 2, 2, 1},
    /* Punta Campanella headland */
    {2, 4, 6, 8, 10, 12, 13, 14, 14, 13, 12, 11, 9, 8, 6, 5, 3, 2, 1},
    /* Salerno and the Monti Picentini */
    {3, 5, 6, 8, 9, 11, 12, 13, 13, 14, 15, 15, 14, 13, 12, 12, 11, 10, 9, 8, 8, 7, 6, 5, 4, 3, 2, 1},
};

/* Per-leg panorama (index 9 = title dusk). Panorama is 1024 px, land on
 * [0, 560), sea on [560, 1024). lm = {id, x/4, scale} */
typedef struct {
    uint8_t seed, mtn_amp, hill_amp, town_density, sun_y, sun_x4;
    uint8_t lm[2][3];
} ob_scenery_t;
static const ob_scenery_t leg_scenery[10] = {
    {11, 14, 10, 40, 18, 180, {{LM_VESUVIO, 58, 2}, {LM_CAMPANELLA, 170, 1}}},  /* Vesuvius towering on the left */
    {23, 44, 16, 14, 14, 200, {{LM_VESUVIO, 180, 1}, {LM_ISCHIA, 226, 1}}},     /* Monte Faito; Vesuvius across the gulf */
    {37, 40, 18, 16, 12, 210, {{LM_VESUVIO, 200, 1}, {LM_ISCHIA, 160, 1}}},
    {41, 30, 14, 34, 10, 220, {{LM_ISCHIA, 156, 1}, {LM_VESUVIO, 222, 1}}},
    {53, 28, 16, 14, 16, 150, {{LM_CAPRI, 150, 2}, {LM_CAMPANELLA, 215, 1}}},   /* Capri ahead from Massa */
    {67, 46, 18, 12, 26, 160, {{LM_LIGALLI, 158, 2}, {LM_CAPRI, 215, 1}}},      /* Li Galli below the SS163 */
    {71, 52, 20, 18, 38, 170, {{LM_LIGALLI, 222, 1}, {LM_SALERNO, 158, 1}}},
    {83, 48, 18, 16, 52, 165, {{LM_SALERNO, 150, 1}, {LM_LIGALLI, 230, 1}}},
    {97, 36, 16, 22, 70, 150, {{LM_SALERNO, 145, 2}, {LM_CAPRI, 238, 1}}},      /* sunset over the Gulf of Salerno */
    {5, 30, 14, 20, 64, 160, {{LM_VESUVIO, 150, 2}, {LM_CAPRI, 232, 1}}},       /* title: Vesuvius at dusk */
};
#endif
