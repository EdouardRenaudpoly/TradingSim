# Aperçu du projet

## Ce que fait ce projet

Un moteur de matching price-time priority écrit en C++20. Il simule le composant central d'un exchange boursier : recevoir des ordres de traders, maintenir un carnet d'ordres par symbole, et matcher les ordres d'achat contre les ordres de vente pour produire des trades.

---

## Architecture

```
Fichier CSV / générateur benchmark
            │
            ▼
    Simulation (ou main)
            │  submit(ordre)
            ▼
     MatchingEngine
            │
      ┌─────┴─────┐
      │           │
  SPSCQueue   MemoryPool
      │           │
      └─────┬─────┘
            │  dispatchOrder(ordre)
            ▼
     PriceLadder (un par symbole)
            │
      ┌─────┴─────┐
      │           │
   bids[]      asks[]      ← tableaux plats indexés par prix
            │
            ▼
    Trades → Metrics / TradeStore (SQLite)
```

**MatchingEngine** est l'orchestrateur. Il possède le memory pool, la SPSC queue, et une map de carnets d'ordres indexée par symbole. Le producteur appelle `submit()` pour pousser un ordre dans la queue. Le consommateur appelle `processAll()` (ou tourne comme thread dédié via `startMatcherThread()`) pour vider la queue et dispatcher chaque ordre au bon carnet.

**PriceLadder** est le carnet d'ordres pour un symbole. Il contient deux tableaux plats `bids_[]` et `asks_[]` où chaque slot est une file FIFO d'ordres à ce niveau de prix. Il implémente la logique de matching pour les six types d'ordres.

**MemoryPool** est un allocateur slab lock-free. Tous les objets `Order` vivent dans un tableau pré-alloué. Allocation et désallocation sont des opérations CAS sur un pointeur atomique — aucun appel à `malloc`.

**SPSCQueue** est un ring buffer lock-free. Le producteur y écrit et le consommateur y lit sans mutex.

---

## Flux de données pour un ordre

1. L'appelant invoque `engine.submit(trader_id, "AAPL", 150.0, 100, BUY, LIMIT)`.
2. `submit()` alloue un `Order` depuis le pool, remplit ses champs, enregistre le timestamp RDTSC, et le pousse dans la SPSC queue.
3. `processAll()` dépile l'ordre, enregistre le temps d'attente en queue (now_tsc − submit_tsc), et appelle `dispatchOrder()`.
4. `dispatchOrder()` trouve ou crée le `PriceLadder` pour "AAPL", prend un snapshot du carnet (pour annoter le slippage), puis route vers le bon chemin de matching selon le type d'ordre.
5. La boucle de matching parcourt le côté opposé du carnet. Pour chaque ordre au repos qui se croise, un `Trade` est produit. Les ordres remplis sont retournés au pool via le callback `reclaim`.
6. Les trades sont annotés avec `mid_at_fill` et `book_imbalance` depuis le snapshot pré-match, puis transmis à `Metrics` et optionnellement à `TradeStore`.

---

## Types d'ordres

| Type | Comportement |
|---|---|
| LIMIT | Inséré dans le carnet au prix spécifié. Matche immédiatement si l'autre côté croise ; le reste repose dans le carnet. |
| MARKET | Balaye le carnet à n'importe quel prix. Ne repose pas — la quantité non remplie est annulée. |
| IOC | Immediate-or-Cancel. Inséré et matché une fois ; le reste non rempli est annulé immédiatement. |
| FOK | Fill-or-Kill. Avant insertion, `canFill()` vérifie si la quantité totale est disponible. Si non, l'ordre est rejeté sans toucher le carnet. |
| POST_ONLY | Rejeté s'il matcherait immédiatement. Garantit que l'ordre repose comme maker. |
| ICEBERG | Seules `peak_size` unités sont visibles dans le carnet. Après chaque remplissage de la tranche visible, une nouvelle tranche est insérée depuis la quantité cachée. Géré via la map `icebergs_` dans `MatchingEngine`. |

---

## Métriques produites

**Par trade :**
- `price`, `quantity`, `buyer_id`, `seller_id`, `symbol`
- `mid_at_fill` — milieu entre meilleur bid et meilleur ask au moment du match
- `book_imbalance` — (bid_depth − ask_depth) / (bid_depth + ask_depth) au meilleur niveau

**Par trader (cumulé sur tous ses trades) :**
- `pnl` — profit/perte réalisé (recettes de vente − coût d'achat, du point de vue de chaque côté)
- `vwap` — prix moyen pondéré par les volumes
- `slippage` — somme de |prix_fill − mid_at_fill| × quantité

**Moteur global :**
- Débit (ordres/sec), fill rate, ratio ordre-to-trade
- Distribution de latence : mean, p50, p95, p99, p99.9, min, max (en nanosecondes, via RDTSC)
- Temps d'attente moyen en queue (soumission → dispatch)

---

## Couche de persistance

Avec `--db <fichier>`, tous les trades et métriques par trader sont écrits dans une base SQLite. SQLite est intégré comme amalgamation vendorisée (`third_party/sqlite/sqlite3.c`) — aucune installation requise.

Le moteur compile et tourne entièrement sans SQLite. Le flag préprocesseur `HAVE_SQLITE3` conditionne tout le code base de données. Si `third_party/sqlite/sqlite3.c` est absent au moment du cmake, le flag n'est pas activé et `TradeStore` compile en no-ops.

Schéma :
```sql
CREATE TABLE trades (
    id INTEGER PRIMARY KEY,
    symbol TEXT, buyer_id INTEGER, seller_id INTEGER,
    price REAL, quantity INTEGER, timestamp_ns INTEGER,
    mid_at_fill REAL, book_imbalance REAL
);
CREATE TABLE trader_metrics (
    trader_id INTEGER PRIMARY KEY,
    trades INTEGER, volume INTEGER,
    pnl REAL, vwap REAL, slippage REAL
);
```

---

## Données de marché réelles

`data/nasdaq_sample.csv` contient 50 000 messages Add Order du flux NASDAQ ITCH du 3 janvier 2003, couvrant MSFT, CSCO, INTC, KLAC, DELL, GE, IBM et SUNW.

`tools/itch_to_csv.py` convertit un fichier texte NASDAQ ITCH v2 brut vers le format CSV du moteur. ITCH est le protocole wire utilisé par NASDAQ pour diffuser son flux complet de carnet d'ordres.

---

## Modèle de threading

**Mode single-thread** (défaut, utilisé dans les tests et `--replay`) : l'appelant soumet et appelle `processAll()` dans le même thread. La SPSC queue sert de buffer de staging mais producteur et consommateur sont le même thread.

**Mode deux threads** (`startMatcherThread()`) : un thread consommateur dédié tourne `matcherLoop()` en continu. Le thread appelant est le producteur et n'appelle que `submit()`. Utilisé dans `--benchmark` et `--replay-benchmark`. C'est la configuration qui exerce vraiment le design lock-free sous vraie concurrence.

---

## Structure du projet

```
engine/
  order_types.hpp        Order, Trade, Side, OrderType, OrderStatus
  memory_pool.hpp        Allocateur slab lock-free (pile de Treiber)
  spsc_queue.hpp         Ring buffer single-producer single-consumer
  latency.hpp            Calibration RDTSC + LatencyTracker
  price_ladder.hpp/cpp   Carnet d'ordres indexé par tableau, logique de matching
  matching_engine.hpp/cpp  Orchestration, état iceberg, stats
  metrics.hpp/cpp        PnL, VWAP, slippage par trader
  simulation.hpp/cpp     Replay CSV
  trade_store.hpp/cpp    Persistance SQLite (optionnel)
tests/
  test_matching_engine.cpp
  test_price_ladder.cpp
  test_spsc_queue.cpp
  test_memory_pool.cpp
  test_metrics.cpp
  test_trade_store.cpp
  test_performance.cpp
tools/
  itch_to_csv.py         Convertisseur NASDAQ ITCH v2 → CSV
data/
  nasdaq_sample.csv      50k ordres NASDAQ réels (3 jan 2003)
  sample_data.csv        Échantillon synthétique minimal
docs/
  overview.md            Aperçu en anglais
  apercu.md              Ce fichier
  internals.md           Décisions techniques en anglais
  technique.md           Décisions techniques en français
```
