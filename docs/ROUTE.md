# The route: Napoli → Vietri sul Mare, 1997

OutBun drives the clockwise tour of the Sorrento peninsula that a Neapolitan would take on a summer Sunday in 1997: out along the Vesuvian coast, onto the **SS145 Sorrentina** at Castellammare, round the tip past Massa Lubrense and down to Nerano, back over the ridge at Sant'Agata to join the **SS163 Amalfitana** at Colli di San Pietro, then down the Amalfi coast to Vietri. The sea stays on the right the whole way.

The road is data in [`src/route.h`](../src/route.h): 92 sections. Each one is a number of 12-segment units with a signed curvature (2 gentle, 4 sweeping, 5–6 *tornante*), a height change, surface flags (town, tunnel, viaduct, cliff on the left, land on the right) and a dominant roadside feature. Every section carries a comment naming the real place it represents. Lengths are not to scale: the real kilometres are compressed so each leg lasts about 20–50 seconds, and busy stretches get more road than empty ones.

## Leg 1 — Napoli → Castellammare di Stabia (SS18, 30 km)

The start gantry stands on Via Marina, with the port cranes on the right and the Torre del Carmine on the left. The road runs through San Giovanni a Teduccio and Portici (the Granatello harbour, the Reggia avenue of palms), past the Miglio d'Oro villas of Ercolano, and over the lava-stone coast to Torre del Greco. Then come the pinewood of the Villa delle Ginestre, the Vesuvian vineyards and Torre Annunziata (Oplontis). The naval shipyard's gantry cranes announce Castellammare. **Vesuvius** fills the left of the sky, twice the size of any other landmark; the Punta Campanella headland lies far ahead.

## Leg 2 — Castellammare → Vico Equense (SS145, 10 km)

The Sorrentina proper: the Terme di Stabia and oleanders, the Pozzano bend under Santa Maria di Pozzano, and the **galleria di Pozzano**. Next are the cliff road under **Monte Faito** (the tallest far ridge of the game), Capo d'Orlando, the galleria Varano, the Scrajo sulphur springs with their lemon terraces, and the bend above Marina di Vico, before the Castello Giusso marks Vico Equense. Across the gulf on the right: Vesuvius and, further out, Ischia.

## Leg 3 — Vico Equense → Meta (SS145, 8 km)

Out of Vico onto the **Ponte di Seiano**, the high viaduct over Marina di Seiano, drawn with stone parapets on both sides above the gorge. Then Seiano, another galleria, and the great **Punta Scutolo** bend with the whole gulf in view. The road descends through the galleria di Alimuri into Meta and the Basilica del Lauro.

## Leg 4 — Meta → Sorrento (SS145, 5 km)

The short, built-up run through Piano di Sorrento and Sant'Agnello: villas, the Cocumella and the walled citrus groves (land on both sides). Corso Italia ends at Piazza Tasso above the Vallone dei Mulini. A limoncello stall waits at the entrance to Sorrento.

## Leg 5 — Sorrento → Nerano (Massa Lubrense, 14 km)

West on Via Capo past the Bagni della Regina Giovanna, through olive and lemon groves to **Massa Lubrense**, where **Capri** rises ahead, large on the horizon, with the Faraglioni and Monte Solaro. The road climbs to Termini at the Punta Campanella path, then drops through two *tornanti* to Nerano and the beach umbrellas of Marina del Cantone.

## Leg 6 — Nerano → Positano (Sant'Agata, SS163, 20 km)

Back up the ridge to **Sant'Agata sui Due Golfi**, where both gulfs are visible and the land lies on both sides of the road, then along the Nastro Verde to **Colli di San Pietro**, the junction with the SS163. The Amalfitana drops in hairpins above Montepertuso, with **Li Galli**, the Sirens' islets, below on the right. After a short galleria comes **Positano**: pastel houses, the dome of Santa Maria Assunta below the road, and the Spiaggia Grande.

## Leg 7 — Positano → Amalfi (SS163, 17 km)

The classic cliff road: Vettica Maggiore, Praiano with San Gennaro's majolica dome, Marina di Praia, and the bridge over the **Fiordo di Furore**. Then a galleria, the Saracen tower of Capo di Conca, Conca dei Marini (Grotta dello Smeraldo) and Vettica's lemons. The striped facade and great stair of the **Duomo di Sant'Andrea** mark Amalfi.

## Leg 8 — Amalfi → Cetara (SS163, 13 km)

Over the arches above **Atrani**, through Castiglione and Minori, along the long beach of **Maiori**, past the Torre Normanna. Then the twisting headland of **Capo d'Orso** and the tower of Erchie bring you to the fishing village of Cetara, home of the anchovy *colatura*. The light turns orange; the Salerno coast grows ahead.

## Leg 9 — Cetara → Vietri sul Mare (SS163, 5 km)

Past the Torre di Cetara, Fuenti, Albori and the Due Fratelli rocks of Marina di Vietri, into the sunset. The blue-and-gold majolica dome of San Giovanni Battista marks the finish in **Vietri sul Mare**, the ceramics town, where the panino is served on a Vietri plate.

## The map

The intro and checkpoint map is drawn from a simplified polygon of the coast (Gulf of Naples, Sorrento peninsula, Amalfi coast and Capri) and a polyline through the towns, both given as longitude/latitude in [`tools/generate_assets.py`](../tools/generate_assets.py). The generator projects them to a 200×154 box at the correct aspect for 40.7° N. The finished legs are drawn red, the current one yellow and the rest white, with a blinking dot for the player.

## Panoramas

The horizon is procedural: a far ridge (Lattari mountains, Monte Faito) and nearer green hills sprinkled with pastel villages, with the land sloping into the sea on the right of a 1024-pixel panorama that turns as the road bends. Landmark silhouettes are stored as height profiles and placed per leg, where you would see them from that stretch: Somma-Vesuvio, Capri, Ischia, Li Galli, Punta Campanella and the Salerno / Monti Picentini coast. The sky follows the time of day, from bright morning in Napoli to sunset at Vietri, by blending 37 palette entries at each checkpoint.
