# Game design

## Flow

`TITLE` (attract mode: the 500 drives itself at dusk) → `MODE` (arcade / network) → `CAR` → [`LOBBY`] → `INTRO` (map and recipe) → `RACE` (countdown, nine legs) → `PANINO` → `RESULTS` → title. START pauses the race (continue / quit).

## Driving

- **Two-speed gearbox.** LO accelerates hard up to 58% of top speed; HI reaches top speed but pulls weakly below 45%. The HUD shows the gear and a rev bar.
- **Top speeds** (display km/h): Fiat 500 196, 126 191, Dyane 187, Beetle 201. Acceleration and grip differ per car (see `car_top`, `car_accel`, `car_grip` in `src/game.c`).
- **Bends** push the car outwards in proportion to curvature × speed, softened by grip. At full speed a hairpin pushes about as hard as full steering lock, so you have to lift or brake.
- **Off-road** beyond the road edge costs speed. The stone parapet on the sea side, the cliff face and the town walls are solid: hitting them bounces you back with sparks and a third of your speed gone. In tunnels and on viaducts the walls are closer.
- **Traffic** (Ape, Vespa, SITA coach) runs slower in both lanes. Hitting one from behind drops you to below its speed, and the drivers sound the horn. The CPU rivals rub panels and slow the faster car.

## Time

The clock starts at the leg-1 allowance plus 6 s. Each checkpoint adds the next leg's allowance (`length / 40 + 5` seconds), so the average speed needed is about 130 km/h, less on the twisty legs because of the fixed 5 s. The test harness bot, which drives much worse than a practised human, finishes with 20 s to spare. When the clock reaches zero the car coasts to a stop and you get the panino you have so far.

## Ingredients and the panino

Each leg has 12 ingredient pickups on the road in the left, centre or right position; the seventh is golden and counts three. CPU rivals can grab them before you. The panino has one layer per ingredient, and each ingredient earns stars: 1 for 1–3, 2 for 4–7, 3 for 8 or more, 27 in all. Without the rosetta from leg 1 the bread is grey and stale.

| Stars | Verdict |
|---:|---|
| 24–27 | IL MIGLIORE DELLA CAMPANIA! |
| 18–23 | SPETTACOLARE! |
| 12–17 | BUONO ASSAI |
| 6–11 | SI PUO' FARE... |
| 0–5 | CHE TRISTEZZA |

## Score

- Distance: speed/128 per tick (about 1,500 points a second at top speed)
- Ingredient: 5,000 (golden: 15,000)
- Arrival at Vietri: 10,000 per second left, plus 50,000 / 30,000 / 15,000 for 1st / 2nd / 3rd
- Panino: 20,000 per star (also on time-up)

The final score goes to the firmware's persistent top five (`OutBun-napoli97`), and the best one is shown on the title screen.

## Rivals

In arcade mode three CPU rivals drive the cars you did not choose. They start on a 2×2 grid, follow the lanes, change lane for traffic, slow down for hairpins, and rubber-band gently: faster when more than 40 segments behind you, slower when more than 50 ahead. In network play, human players take slots first and the CPU keeps the rest; a player who stays silent for 4 s is handed to the CPU.

## Why the route is shaped like this

A leg's length follows its real distance and how busy it is. The dramatic stretches — Pozzano, Seiano, Punta Scutolo, the tornanti down to Nerano and Positano, Furore, Capo d'Orso — have the tightest curvature and the tunnels. Town stretches bring traffic, lamps, houses and beaches; open coast brings pines, agaves, lemon groves and shrines. See [ROUTE.md](ROUTE.md).
