# Technique — Décisions d'architecture C++ et HFT

---

## Table des matières

1. [Mutex vs atomic](#1-mutex-vs-atomic)
2. [Ordonnancement mémoire — acquire, release, seq_cst](#2-ordonnancement-mémoire--acquire-release-seq_cst)
3. [La SPSC queue](#3-la-spsc-queue)
4. [Le memory pool (pile de Treiber)](#4-le-memory-pool-pile-de-treiber)
5. [Lignes de cache, false sharing et alignas(64)](#5-lignes-de-cache-false-sharing-et-alignas64)
6. [Price ladder — tableau plat vs std::map](#6-price-ladder--tableau-plat-vs-stdmap)
7. [Liste chaînée intrusive pour les files par niveau de prix](#7-liste-chaînée-intrusive-pour-les-files-par-niveau-de-prix)
8. [RDTSC — mesurer le temps sans syscall](#8-rdtsc--mesurer-le-temps-sans-syscall)
9. [Callbacks template vs std::function](#9-callbacks-template-vs-stdfunction)
10. [Tableau plat pour la lookup ordre-to-niveau](#10-tableau-plat-pour-la-lookup-ordre-to-niveau)
11. [Symbole en uint64_t](#11-symbole-en-uint64_t)
12. [Threading producteur/consommateur](#12-threading-producteurchnsommateur)
13. [Ordres iceberg et spikes de latence p99](#13-ordres-iceberg-et-spikes-de-latence-p99)
14. [Lire les résultats de benchmark](#14-lire-les-résultats-de-benchmark)
15. [Fonctionnalités C++20 utilisées](#15-fonctionnalités-c20-utilisées)

---

## 1. Mutex vs atomic

Quand un thread appelle `pthread_mutex_lock()` sur un verrou contesté, l'OS marque le thread comme bloqué, le retire de la run queue, et fait un context switch vers un autre thread. À la libération du verrou, le thread bloqué est replanifié — mais ses registres CPU doivent être restaurés et son cache L1/L2 est froid parce qu'un autre thread a utilisé le CPU entretemps. Coût total : des dizaines de milliers de nanosecondes.

`std::atomic<T*>` se traduit en instruction `lock cmpxchg` sur x86. L'opération est une seule instruction matérielle : aucun syscall, aucune intervention du kernel, aucun context switch. Coût : 5–20 cycles CPU.

Le memory pool et la SPSC queue sont les seuls endroits du moteur qui utilisent des atomiques. Tout le reste — I/O fichier, SQLite, initialisation — utilise de la synchronisation standard car ces chemins ne sont pas sensibles à la latence.

Code concerné : `engine/memory_pool.hpp`, `engine/spsc_queue.hpp`.

---

## 2. Ordonnancement mémoire — acquire, release, seq_cst

Les CPUs modernes et les compilateurs réordonnent les instructions pour améliorer le débit. En code multi-threadé, cela peut amener un thread à observer des écritures mémoire dans un ordre différent de celui dans lequel un autre thread les a produites.

### memory_order_relaxed

Aucune garantie d'ordre au-delà de l'atomicité. Utilisé pour des compteurs qu'aucun autre thread ne lit immédiatement : `next_id_.fetch_add(1, std::memory_order_relaxed)`.

### memory_order_release / memory_order_acquire

`release` sur une écriture : toutes les écritures précédentes sont garanties visibles pour tout thread qui fait ensuite un `acquire` load sur le même emplacement.

Dans la SPSC queue :
```cpp
// Producteur
buffer_[h] = item;
head_.store(next_h, std::memory_order_release); // "l'item est prêt"

// Consommateur
if (t == head_.load(std::memory_order_acquire)) // "je peux maintenant lire l'item"
    return std::nullopt;
T item = buffer_[t];
```

Sans release/acquire, le CPU pourrait réordonner `head_.store` avant `buffer_[h] = item`. Le consommateur lirait l'index mis à jour mais verrait des données corrompues dans le slot. Cela arrive en pratique sur les CPU ARM.

### memory_order_seq_cst

Séquentiellement cohérent. Génère une instruction `mfence` sur x86, qui vide le store buffer du CPU (~20–40 cycles). C'est le défaut quand aucun ordre n'est spécifié — correct mais coûteux. Seuls les endroits qui nécessitent un ordre total entre plusieurs atomiques l'utilisent.

Code concerné : `engine/spsc_queue.hpp` lignes 16–35, `engine/memory_pool.hpp`.

---

## 3. La SPSC queue

La queue est un ring buffer de taille fixe. Le producteur incrémente `head_`, le consommateur incrémente `tail_`. Quand ils sont égaux la queue est vide ; quand `head_ + 1 == tail_` elle est pleine.

**Pourquoi pas std::queue :** `std::queue` alloue un nœud par élément via `malloc`. Sous contention, `malloc` prend un verrou interne. Sur le chemin critique c'est inacceptable. Le ring buffer est une allocation fixe — `push` et `pop` sont une vérification de bornes, un accès tableau, et un atomic store.

**Capacité puissance de deux :** L'enroulement d'index utilise `& MASK` au lieu de `% Capacity`. Le modulo se compile en division entière (20–30 cycles). Un masque de bits est une seule instruction AND (1 cycle). Contraint par `static_assert((Capacity & (Capacity - 1)) == 0)`.

**Pourquoi SPSC et pas MPSC/MPMC :** Une queue multi-producteur ou multi-consommateur nécessite des boucles CAS sur head et tail. SPSC est le cas minimal : exactement un thread écrit `head_`, exactement un lit `tail_`. Ils ne se disputent jamais le même atomique.

Code concerné : `engine/spsc_queue.hpp`.

---

## 4. Le memory pool (pile de Treiber)

### Slab pré-alloué

Les 65 536 objets `Order` sont alloués à la construction dans un seul tableau contigu : `std::array<Order, 65536> storage_`. Allocation et désallocation sont ensuite des opérations O(1) sur des pointeurs, sans syscalls et sans interaction avec `malloc`.

### Liste libre lock-free

La liste libre est une pile implémentée avec un seul pointeur atomique `head_`. Chaque `Order` libre utilise son champ `next` pour pointer vers le prochain slot libre.

**Allouer :**
```cpp
T* old = head_.load(memory_order_acquire);
while (old) {
    if (head_.compare_exchange_weak(old, old->next, ...))
        return old;  // CAS réussi : on a pris l'élément du dessus
    // CAS échoué : un autre thread nous a devancés ; réessayer avec le nouveau head
}
```

**Désallouer :**
```cpp
node->next = head_.load(memory_order_relaxed);
while (!head_.compare_exchange_weak(node->next, node, ...));
```

`compare_exchange_weak` se traduit en `lock cmpxchg` : vérifie atomiquement que `head_` vaut toujours `old`, et si oui le remplace par `old->next`. Si un autre thread a modifié `head_` entre temps, le CAS échoue et la boucle réessaie.

**compare_exchange_weak vs strong :** `_weak` peut échouer spurieusement sur les architectures LL/SC (ARM). Il est préféré dans les boucles de retry car le matériel l'implémente plus efficacement. `_strong` garantit l'absence d'échecs spurieux mais coûte plus.

Code concerné : `engine/memory_pool.hpp`.

---

## 5. Lignes de cache, false sharing et alignas(64)

Le CPU lit la mémoire par blocs de 64 octets appelés lignes de cache. Quand le cœur A écrit à un emplacement, le protocole de cohérence de cache (MESI) invalide cette ligne de cache dans le L1 du cœur B. Le cœur B doit recharger la ligne à son prochain accès, même s'il lisait une variable différente qui partageait par accident les mêmes 64 octets.

### alignas(64) sur head_ et tail_

```cpp
alignas(64) std::atomic<std::size_t> head_{0};
alignas(64) std::atomic<std::size_t> tail_{0};
```

Sans ça, `head_` et `tail_` seraient adjacents en mémoire, partageant une ligne de cache. Chaque écriture sur `head_` par le producteur invaliderait `tail_` dans le cache du consommateur, et vice versa — un cache miss par push et par pop sans aucune raison logique. Les forcer sur des lignes séparées élimine ça.

### alignas(64) sur Order

```cpp
struct alignas(64) Order { ... };
static_assert(sizeof(Order) == 64);
```

Chaque `Order` occupe exactement une ligne de cache. La boucle de matching parcourt une liste chaînée d'ordres à un niveau de prix. Avec cette disposition, charger chaque ordre nécessite exactement un fetch de ligne de cache. Sans alignement, deux ordres adjacents pourraient chevaucher une limite de ligne de cache, nécessitant deux fetches par ordre.

Code concerné : `engine/order_types.hpp` ligne 19, `engine/spsc_queue.hpp` lignes 47–48.

---

## 6. Price ladder — tableau plat vs std::map

### std::map (rejeté)

`std::map<double, queue<Order*>>` est un arbre rouge-noir. Chaque nœud est une allocation heap séparée, dispersée en mémoire. Trouver un niveau de prix est O(log n) avec des comparaisons pointer-chased — chaque comparaison peut être un cache miss. La boucle de matching parcourt les niveaux vers l'intérieur depuis le meilleur prix : dans un arbre, le parcours in-order suit des pointeurs à chaque étape, chacun potentiellement un cache miss.

### Tableau plat (utilisé)

```cpp
std::vector<Level> bids_(num_levels_);
std::vector<Level> asks_(num_levels_);
// accès : bids_[priceToIdx(price)]
```

`priceToIdx` est une multiplication et une addition. L'accès tableau est un load. Si le ladder est en cache, c'est 1–4 cycles au total.

La boucle de matching parcourt des slots adjacents vers l'intérieur depuis le meilleur prix. C'est un scan mémoire séquentiel — le prefetcher matériel reconnaît le patron et charge la prochaine ligne de cache avant qu'elle soit nécessaire.

**Coût mémoire :** [0, 500] avec tick 0.01 → 50 001 niveaux × 2 côtés × 32 octets ≈ 3.2 Mo par carnet. Tient dans le cache L3 des processeurs modernes.

Code concerné : `engine/price_ladder.hpp`, `engine/price_ladder.cpp`.

---

## 7. Liste chaînée intrusive pour les files par niveau de prix

Chaque niveau de prix est une file FIFO d'ordres au repos. `std::list<Order*>` allouerait un nœud de liste par élément via `malloc`, qui sous contention prend un verrou interne.

Une liste intrusive stocke le pointeur de lien à l'intérieur de l'élément lui-même :

```cpp
struct alignas(64) Order {
    // ... champs trading ...
    Order* next;  // lien intrusif : liste libre dans le pool, ou file par niveau de prix
};
```

Insérer un ordre dans un niveau de prix est deux assignations de pointeurs : `tail->next = order; tail = order`. Aucune allocation. L'`Order` vit déjà dans le memory pool — on réutilise le champ pointeur qu'on a déjà payé.

**Double usage de `next` :** Quand un ordre est dans la liste libre il lie les slots libres. Une fois alloué et inséré dans un niveau de prix il lie les ordres à ce niveau. Les deux usages ne se chevauchent jamais : un ordre ne peut pas être simultanément libre et dans le carnet.

Code concerné : `engine/order_types.hpp` ligne 32, `engine/price_ladder.cpp` lignes 7–20.

---

## 8. RDTSC — mesurer le temps sans syscall

`clock_gettime(CLOCK_MONOTONIC)` coûte ~20–50 ns même avec l'optimisation vDSO (qui évite un syscall complet en mappant l'horloge dans l'espace utilisateur). Il lit une structure de données kernel depuis la mémoire et convertit en nanosecondes.

`__rdtsc()` lit le registre TSC (timestamp counter) du CPU directement en une instruction. Aucun accès mémoire, aucune intervention du kernel. Coût : ~1 cycle CPU.

**Calibration :** RDTSC retourne des cycles, pas des nanosecondes. On calibre une fois au démarrage en comparant les ticks RDTSC contre `steady_clock` sur 10 ms pour dériver `cpu_ghz`, puis on convertit : `nanosecondes = tsc_delta / cpu_ghz`.

**Limite :** Si le CPU change de fréquence entre la calibration et la mesure (économie d'énergie sur laptop), la conversion dérive. Sur serveur avec fréquence CPU fixe ce n'est pas un problème.

Code concerné : `engine/latency.hpp`.

---

## 9. Callbacks template vs std::function

Le callback `reclaim` dans la boucle de matching est appelé une fois par ordre rempli. Avec `std::function` :

```cpp
std::vector<Trade> match(std::function<void(Order*)> on_filled = {});
```

Construire un `std::function` depuis un lambda qui capture des variables alloue un objet heap pour stocker les captures. Chaque appel dispatche via un pointeur de fonction virtuelle — une branche indirecte que le branch predictor du CPU ne peut pas inliner de façon fiable.

Avec un paramètre template :

```cpp
template<typename F = std::nullptr_t>
std::vector<Trade> match(F on_filled = nullptr) {
    // ...
    if constexpr (!std::is_null_pointer_v<std::decay_t<F>>)
        on_filled(bid);
}
```

Quand `F` est un type lambda concret, le compilateur voit la fonction exacte à l'instantiation et inline le corps directement dans la boucle de matching. Zéro allocation heap, zéro branche indirecte.

Quand `F` est `std::nullptr_t` (le défaut), la branche `if constexpr` est éliminée au moment de la compilation. Le code machine pour le cas sans callback ne contient aucune trace de callback.

Code concerné : `engine/price_ladder.hpp`, section template.

---

## 10. Tableau plat pour la lookup ordre-to-niveau

Quand un ordre est annulé, le moteur doit savoir à quel niveau de prix il se trouve. Avec `unordered_map<uint64_t, int32_t>` : hasher la clé, sonder un bucket, suivre un pointeur — 20–50 ns.

Avec un tableau plat :

```cpp
std::vector<int32_t> order_level_;  // taille = pool_capacity
// écriture : order_level_[order->id % order_cap_] = idx;
// lecture  : idx = order_level_[order->id % order_cap_];
```

Un modulo et un accès tableau — 1–4 cycles.

**Pourquoi c'est sans collision :** Les IDs d'ordres sont assignés séquentiellement. Le pool a 65 536 slots et au maximum 65 536 ordres vivants simultanément. Si l'ordre ID 65 537 est vivant, l'ordre ID 1 est forcément mort (son slot pool a été réutilisé). Donc `order_level_[65537 % 65536]` est sûr : l'ancien occupant de ce slot a déjà été nettoyé.

Code concerné : `engine/price_ladder.hpp` (`order_level_`), constructeur de `engine/price_ladder.cpp`.

---

## 11. Symbole en uint64_t

Les symboles de trading sont des chaînes ASCII courtes (1–8 caractères). Le moteur stocke un `PriceLadder` par symbole dans une hash map.

Avec `std::string` comme clé : le hash appelle `strlen` puis itère caractère par caractère. Même avec le SSO (pas d'allocation heap pour les chaînes courtes), la fonction de hash itère octet par octet.

Avec `uint64_t` :
```cpp
static uint64_t symbolToKey(const char* s) noexcept {
    uint64_t key = 0;
    std::memcpy(&key, s, std::min(strlen(s), size_t(8)));
    return key;
}
```

Les 8 octets du symbole sont réinterprétés comme un seul entier 64 bits. Hasher un `uint64_t` est une seule opération multiply-shift (~2 cycles). La lookup dans la map est par ailleurs identique.

Code concerné : `engine/matching_engine.hpp` (`symbolToKey`, `books_`).

---

## 12. Threading producteur/consommateur

En mode single-thread (`processAll()` appelé par le même thread qui soumet), les structures de données lock-free ne sont jamais réellement en contention. Le design n'est exercé sous vraie concurrence que quand `startMatcherThread()` est utilisé.

En mode deux threads, le producteur (thread principal) et le consommateur (thread matcher) tournent simultanément. Leur seul état partagé est la SPSC queue. L'ordonnancement acquire/release assure que le consommateur voit les ordres complètement écrits. Le placement `alignas(64)` de `head_` et `tail_` empêche le false sharing entre les deux cœurs.

**yield vs busy-spin :** Quand la queue est vide, `matcherLoop()` appelle `std::this_thread::yield()` pour céder le CPU à d'autres threads. En production HFT, le consommateur ferait du busy-spin (pas de yield) pour minimiser la latence de réveil, au coût de 100% d'utilisation CPU sur un cœur dédié. Le yield est le bon compromis pour une démo qui partage une machine.

**Arrêt propre :** `stopMatcherThread()` appelle `request_stop()` sur le `jthread`, puis le join. Le join garantit que tous les ordres en queue sont traités avant que le thread principal lise les stats.

Code concerné : `engine/matching_engine.cpp` (`matcherLoop`, `startMatcherThread`, `stopMatcherThread`).

---

## 13. Ordres iceberg et spikes de latence p99

Quand la tranche visible d'un iceberg est entièrement remplie, `dispatchOrder()` alloue un nouvel `Order` depuis le pool, copie les champs, met `remaining = min(peak_size, hidden_remaining)`, et le réinsère dans le carnet via la liste `renewals`. Si un ordre market agressif balaie plusieurs tranches, chaque fill déclenche une autre réinsertion et un nouveau cycle de matching. Un seul appel à `dispatchOrder()` exécute plusieurs cycles de matching.

C'est pourquoi le p99 est deux ordres de grandeur au-dessus du p50 dans le benchmark synthétique (41 962 ns vs 394 ns). Avec uniquement des ordres LIMIT, chaque dispatch est un seul cycle de matching et p99 descend près de p50. Le benchmark NASDAQ montre un p99 beaucoup plus bas (893 ns) car le vrai order flow contient très peu d'icebergs.

Code concerné : `engine/matching_engine.cpp` (`dispatchOrder`, lambda `reclaim`).

---

## 14. Lire les résultats de benchmark

### Synthétique vs données réelles

Le benchmark synthétique distribue les prix uniformément sur une plage de $4 avec les six types d'ordres, maximisant les fills et les replenishments iceberg. C'est un stress test worst-case.

Le benchmark NASDAQ rejoue des ordres réels qui se regroupent près du prix de marché et sont majoritairement de type LIMIT. La boucle de matching s'arrête plus tôt (moins de niveaux traversés), et les icebergs sont rares. C'est pourquoi les données réelles montrent un débit 3× supérieur et un p99 bien plus bas que le synthétique.

Les deux chiffres sont utiles : le synthétique donne une baseline worst-case reproductible ; le NASDAQ valide la performance sous conditions de marché réalistes.

### Temps d'attente en queue

"Avg queue wait" est le temps de `submit()` au moment où `processAll()` dépile l'ordre. En mode single-thread il est quasi nul. En mode deux threads une valeur élevée signifie que le consommateur ne suit pas le rythme du producteur — la queue s'accumule.

### Distribution de latence

- **p50** : temps de dispatch médian — lookup du niveau de prix + boucle de matching pour un ordre typique.
- **p95** : ordres plus lourds, léger cache miss sur un niveau de prix froid.
- **p99** : cycles de replenishment iceberg, ou un carnet de symbole froid.
- **p99.9 / max** : préemption du scheduler OS, page fault. Non significatif pour caractériser la performance.

---

## 15. Fonctionnalités C++20 utilisées

### std::jthread et std::stop_token

`std::jthread` remplace `std::thread` + `std::atomic<bool> stop_flag_` pour le thread matcher. Deux améliorations :

1. **Join automatique à la destruction** : le destructeur de `jthread` appelle `request_stop()` puis `join()` automatiquement. Le destructeur explicite `~MatchingEngine()` n'est plus nécessaire — celui généré par le compilateur est correct.

2. **Annulation coopérative via `std::stop_token`** : au lieu de vérifier un `atomic<bool>` brut, la boucle matcher reçoit un `std::stop_token` et appelle `stop.stop_requested()`. Le signal d'arrêt se propage via le token, pas via une variable atomique partagée qui pourrait être mal utilisée.

```cpp
// Avant (C++17)
std::thread        matcher_thread_;
std::atomic<bool>  stop_flag_{false};

void matcherLoop() {
    while (!stop_flag_.load(std::memory_order_relaxed) || !queue_.empty()) { ... }
}

// Après (C++20)
std::jthread matcher_thread_;

void matcherLoop(std::stop_token stop) {
    while (!stop.stop_requested() || !queue_.empty()) { ... }
}
```

Code concerné : `engine/matching_engine.hpp`, `engine/matching_engine.cpp`.

### Concepts — IntrusiveNode

```cpp
template<typename T>
concept IntrusiveNode = requires(T t) { { t.next } -> std::convertible_to<T*>; };

template<IntrusiveNode T, std::size_t Capacity>
class MemoryPool { ... };
```

`MemoryPool` requiert que son type élément expose un pointeur `T* next` pour le chaînage intrusif de la liste libre. Avant, c'était documenté dans un commentaire ; le concept rend la contrainte vérifiable à la compilation. Passer un type sans pointeur `next` produit une erreur claire à l'instantiation plutôt qu'un échec cryptique à l'édition de liens.

Code concerné : `engine/memory_pool.hpp`.

### [[likely]] et [[unlikely]]

```cpp
if (next_h == tail_.load(std::memory_order_acquire)) [[unlikely]]
    return false;  // queue pleine — rare sur un système sain

while (old) [[likely]] {  // l'épuisement du pool est le cas exceptionnel
```

Ces attributs indiquent au compilateur quelle branche est prise dans le cas commun. Le compilateur organise le code machine pour que le chemin chaud soit une ligne droite (branche non prise, pas de flush du pipeline). Sur x86 cela affecte aussi les hints de prédiction de branche statique dans l'encodage des instructions.

Code concerné : `engine/spsc_queue.hpp`, `engine/memory_pool.hpp`.
