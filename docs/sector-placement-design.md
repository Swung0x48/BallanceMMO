# Sector placement: one object per PH row

Status: **built** (track 1 of the pooled-module fix): S1-S10 and the
records (S11), HEAD fa8218d; **§12 is the consolidated as-built record**
(commits, what the code does where it differs from §2-§5, the numbers,
§9's answers, what is open). The step notes: S1 and S2 in 2.2 / 2.3, S9 in
2.5, S4 (inside resync A5) in 2.6, S5 in 5, S3 in 7.3, S6, S7, HU1 and S8 in
7.4, S10 in 7.6. Protocol 10 / journal v4 went to resync A3 and 11 / v5 to trafo T2, so S5 took protocol 12 / journal v6 (§5, §7.1 and §8 corrected); revision 2
after two reviews (answers in §10). Base: HEAD 4882ec2 (R2) plus the
uncommitted step P in the working tree; line numbers are the working tree's on
2026-10-23 and drift with step P, so each one is given with the function or
block it points at.

The user's decision this implements (rollback-coverage audit §5, item #9):
the invariant "a player entering the next sector must not change any physics
state of the previous sector", built with the diagnosis's option B - every
world, at its anchor, gives every PH row whose object an earlier row already
placed at a different matrix its own copy. It also carries the fix of the
related late-joiner finding (#16), because that fix decides which world a
joiner builds, and the fix of the retail second-session gap (§2.5), because
the placement's retail lifetime depends on it.

Sources: the diagnosis `audit2/pooled-modules.md`, the audit
`audit/rollback-coverage-audit.md` and the SimTool runs made for this design
in `design/track1/` (named where used), all under the session scratchpad
`C:/Users/GEEKER~1/AppData/Local/Temp/claude/D--repos-BallanceMMO/769d21ff-c9e2-42a0-a69a-d95ce0b06545/scratchpad/`.

Path key:

| key | file |
|---|---|
| PW | `BallanceMMOServer/sim/physics_world.cpp` |
| SC | `BallanceMMOServer/sim/session_client.cpp` |
| SRN | `BallanceMMOServer/sim/session_runner.hpp` / `.cpp` |
| SV | `BallanceMMOServer/server.cpp` |
| CH | `BallanceMMOClient/session/core_host.cpp` |
| PSC | `BallanceMMOClient/session/physics_session_client.cpp` (legacy retail path) |
| MC | `BallanceMMOClient/BallanceMMOClient.cpp` |
| CA | `BallanceMMOClient/automation/client_automation.cpp` |
| ST | `BallanceMMOServer/sim/sim_tool.cpp` |
| SU / SP | `BallanceMMOCommon/include/session/sector_union.hpp` / `sector_pacer.hpp` |
| SR | `BallanceMMOCommon/include/game/script_replay.hpp` |
| HS | `BallanceMMOCommon/include/game/session_host_support.hpp` |
| CK2 | `submodule/Ballanced/Source/CK2/src/` |
| SDK | `build-client-stock/_deps/virtools_sdk-src/Include/` (the retail Virtools SDK headers) |

## 1. The problem and the rule

### 1.1 Symptom

Every original level (L1-L13) pools PH objects across sectors; the arena does
not. Under the sector union (protocol 3: every side runs every sector any
member entered, nothing is deactivated), a later sector's pass re-places a
pooled object that an earlier sector still uses. Measured with SimTool
(diagnosis §1.2):

| case | with the later sector | without |
|---|---|---|
| L8 P_Modul_30 see-saw, s1, plank awake, sector 2 at tick 30 | sags 1.158 m | 0.085 m |
| L8 s1 plank, ball only at the s2 site, sector 2 at tick 300 | s1 plank drops 1.160 m when woken | 0.000 m |
| L8 P_Modul_29 rope bridge (s2), sector 4 | planks flung up to 2049.9 m | sags 8.6 m |
| L10 P_Modul_41, sector 4 | falls 464.68 m | 0 m |
| L11 P_Modul_26 sack, sector 3 | swings along x, up to 10.32 m off | swings in z only |
| L11 P_Modul_01 (s1), player at the s4 site | woken from about 965 m | not woken |
| L8 s2/s4 see-saw and s4 bridge sites | no body: ball falls 138.2 m | - |

Four classes: jointed modules of the earlier sector lose their pivot; the
later sector's instance does not exist; fans, trafos and extras leave the
earlier sector; wake triggers move. The worlds agree with each other (same
writes at the same lockstep tick), so no rollback or correction fires: it is
a gameplay bug that no mismatch counter sees (diagnosis §2.5).

### 1.2 Cause

1. **The builder pools by design.** `Levelinit_build / Count PH` sets
   `PH_Groups.Amount` to the largest number of rows of a type in any one
   sector; `Copy Objects` makes Amount-1 copies (`Object Copy`, Dynamic=1,
   then `TT Save IC` with hierarchy, then `Add To Group`); `set MF-Objects in
   Array` deals the type group's members to the PH rows (column 3) and loops,
   so a later sector's rows re-use earlier sectors' objects (diagnosis §2.1).
2. **Vanilla tears down before it moves.** A crossing deactivates the old
   sector (module False branch: Hinge Shutdown, Unphysicalize, TT Restore IC)
   and only then activates the new one (diagnosis §2.2).
3. **The union never tears down** (N2, `docs/collision-overhaul-design.md`
   9.25/9.28; SU:17-25). So the later pass's `activate Type 1` runs Set World
   Matrix (Hierarchy=1) + Show + Activate Script(reset) on a live module: the
   unphysicalized frames (HingeFrame, the proximity's ObjectB) move with the
   matrix, Physicalize short-circuits on the existing body
   (`Physicalize.cpp:104-116`), and Set Physics Hinge deletes and rebuilds the
   constraint from the moved frame (`PhysicsHinge.cpp:118-131, 152-158`).
   Type 2/3 rows (boxes, domes, prop balls) only get the matrix; trafos and
   extras just move (diagnosis §2.3-2.4).

### 1.3 The invariant

**I1 (the user's rule (b)).** For every sector S and every later pass of a
sector T != S: the bodies, constraints, force controllers, wake triggers and
placed non-physical objects (trafos, extras, fan frames) of S are
bit-identical, tick by tick, to a run in which T never activates.

**I2 (how B implements it).** After the anchor, every PH row names its own
object, except rows whose matrix is bit-identical to an earlier placement of
the same object (the same-pose pairs, which stay pooled by the user's
decision); such a row names the **same object** as its holder row. A pass
then touches only objects no other sector holds at another pose.

**I3 (one world).** Every world - the server's, every headless and retail
client, every late joiner, SimTool - builds the same placement from the same
level file: same rows copied, same names, same groups and attributes in the
same list order, verified by a signature that every world computes and the
server compares at SessionReady, whatever the session's options (§2.2, §5).

### 1.4 Facts established for this design

All from offline SimTool runs on the agent install
(`build-agent/BallanceMMOServer/BallanceMMOSimTool.exe --root
C:/Users/geekerwan/Desktop/Ballance-Agent`), the Ballanced sources, the SDK
headers and the retail run logs under `maps/arena-test/`.

| # | fact | evidence |
|---|---|---|
| F1 | The builder's copies get unique 3D-entity names with a numeric suffix on every copied entity (`P_Modul_41_MF001`, its body `P_Modul_41001`, `P_Modul_41_HingeFrame001`; `P_Extra_Point_Frame4001`); their scripts keep the original's name (`P_Modul_41_MF Script` on all seven). So the class dependencies include `CK_DEPENDENCIES_COPY_OBJECT_NAME` and `..._UNIQUENAME` for entities (CK2/CKObject.cpp:164-197, case 2: `GetObjectSecureName`, which depends on the names already in the world). | `design/track1/l8_scripts.txt` (`--list-scripts "*"`), `l8_m41.txt` (`--dump-script "P_Modul_41_MF Script"`) |
| F2 | Meshes are **shared**, not copied: all seven P_Modul_41 copies physicalize `P_Modul_41_Col1_Mesh`/`Col2_Mesh` and the named surface `P_Modul_41_Mesh`; Object2 is the shared `FixCube`. (The diagnosis's "meshes remapped" is not what the builder's copies show.) | `l8_m41.txt` |
| F3 | The per-type `*_Init` scripts are owned by `All_P_Modul_XX` objects, not by the MF objects, so a copy carries no init script; they do no per-member work (they resolve shared arrays and groups). The child scripts (`P_Modul_18_Particle Script`, `P_Extra_Point_Frame* script`) are on copied children and are activated by their MF script through internal (remapped) Script references. | `l8_scripts.txt`; `audit/d_P_Modul_18_MF_Script.txt`, `d_P_Modul_18_Init.txt`, `d_P_Extra_Point_Init.txt` |
| F4 | `Levelinit_build` holds **two** Object Copy blocks, with different dependency objects: id 37186 in the graph `Copy Objects` (its Out goes to `TT Save IC` 37241; its pin 0 reads `Copy Objects` local 1, `80 88 24 f3 ...`) makes the PH copies; id 37657 in `init and load Checkpoints` (its Out goes to `fill Checkpoint-Arary`; `c0 89 24 f3 ...`) copies the PC_TwoFlames checkpoint objects. Both flag words print as pointers, so their bits are not settled offline (§9 Q1). | `audit/d_Levelinit_build.txt:788-861, 894, 1147-1167`; `d2_Levelinit_build.txt:879, 1185` |
| F5 | `CKBeObject::Copy` copies scripts (class bit 1), attributes (bit 2) and group membership (bit 4) only when the dependency asks (CK2/CKBeObject.cpp:753-811). `CKDependenciesContext::Copy` creates the copies **in XHashTable order of the source ids**, so the creation order inside one copy depends on the world's ids; if any Copy or Remap fails it destroys every copy it made (CK2/CKDependencies.cpp:110-173); it adds the copies to the level scene with the original's scene flags minus ACTIVE and activates a copy if the creation asks for it or **if the original is active** (:175-210). The created objects are enumerable with `FillRemappedDependencies()` (CK2 include/CKDependencies.h:149; SDK CKDependencies.h:146). | sources |
| F6 | The Trafo Manager's `Get Nearest In Group` reads the group `Trafos` that `Init Ingame / create TrafoGroup` makes on every level start: `Object Create("Trafos", dynamic)` then `Objects With Attribute Iterator(attribute 53)` into it; `set Trafo-Attribute` then puts attribute 53 with "paper"/"wood"/"stone" on the three balls. So a trafo copy is found only if it carries attribute 53 when the group is built, or is added to the group. Attribute 53 is the Ballanced process's type number; attribute types are numbered per process in registration order (CK2/CKAttributeManager.cpp:14, 795-799). | `audit/d_Gameplay_Ingame.txt:118-123, 765-830` |
| F7 | `CKAttributeManager::AddAttributeToObject` and `CKGroup::AddObject` **append** (CK2/CKAttributeManager.cpp:488-506, CKGroup.cpp:7-16), so attribute-iterator and group order are the order of the calls. | sources |
| F8 | Attribute 54 ("Sector") is a build-time attribute: `Set Sector Attribute` puts it on the placeholders of the `Sector_0N` groups and `Replace PH` reads it into the PH rows, then deletes the placeholders. Nothing reads it at run time. | `audit/d_Levelinit_build.txt:896-1030` |
| F9 | The level's own sector-1 activation is **not** pending at the anchor: `Init Ingame` writes `IngameParameter[0][1] = 1` and activates the manager (`Activate Script` 25731, reset) two frames later. On L8 the manager is first active after the step of tick 2 and the first modules are born at tick 7. A union pass owed at the anchor starts at tick 2 (`world: sector +2 at tick 2 (player 2, stamped 0)`), and sector 1 is never walked: only the s2 Wippe exists. A pass started by the union keeps the manager active through its delay-4 link (two owed passes start 7 ticks apart). | `design/track1/l8_s1_trace.txt`, `l8_p2s2.txt`, `l8_p2s2s3.txt`; `audit/d_Gameplay_Ingame.txt:687-800` |
| F10 | At the retail anchor the manager is inactive: all 197 retail anchors in `maps/arena-test` (L11 and the arena; every `BallanceMMOClient/INFO ... script replay:` line) print `Gameplay_SectorManager(off)` in the selection line written at the anchor (SR:351). No retail log holds two anchors in one level load. | `grep -rha "BallanceMMOClient/INFO.*script replay:" maps/arena-test` |
| F11 | A constraint to the shared fixed `FixCube` does not fuse simulation units: IVP skips `physical_unmoveable` cores when it merges units (`ivp_sim_unit.cxx:541, 622`), and engine change #5 already made the movement check per unit for exactly this coupling. | sources |
| F12 | Module scripts look up no 3D entity by name: the only string-to-object conversions in the module scripts are to groups and a sound. | scan of `audit/d_P_*Script*.txt` |
| F13 | CK reuses ids LIFO, and only ids freed by a `CK_DESTROY_FREEID` destroy (CK2/CKObjectManager.cpp:223-224, 342-383). A copy's CK_ID, and an original's, depend on the world's object history, which differs between a retail client that played for a while and a fresh server; a retail copy can get an id **below** its original's. | sources |
| F14 | P_Modul_01 chooses Physicalize or Unphysicalize once per script start, from `Get Cell(row 0, column 4)` (the activation phase) through a Binary Switch; its TT Scaleable Proximity only wakes the Pusher and switches itself off. There is no proximity-driven Physicalize/Unphysicalize cycle. | `audit/d_P_Modul_01_MF_Script.txt:1-72` |
| F15 | The mod already loads a level the way `Menu_Start` does: `CurrentLevel[0,0] = level`, then the `Load Level` message to the owner of `Event_handler` (CA:231-257). | source |

The plan (§2.2), run as a prototype over the PH dumps of every level
(`design/track1/plan_proto.py`, output `plan_counts.txt`; the dumps print
matrices with `%g`, so "same pose" is textual there and bitwise in the real
plan):

| level | PH rows | copies | of which extra / trafo / fan / module / prop | same-pose (kept pooled) | selected scripts added (est.) | movable bodies added (est.) |
|---|---|---|---|---|---|---|
| L1 | 51 | 23 | 10 / 2 / 0 / 0 / 11 | r38 shares r37's copy (P_Trafo_Wood_MF) | 0 | 11 |
| L2 | 62 | 26 | 15 / 8 / 3 / 0 / 0 | - | 6 | 0 |
| L3 | 53 | 17 | 8 / 8 / 0 / 1 / 0 | r16 = r8 (P_Dome_MF) | 1 | 1 |
| L4 | 75 | 29 | 16 / 11 / 1 / 0 / 1 | - | 2 | 1 |
| L5 | 55 | 27 | 11 / 9 / 1 / 6 / 0 | - | 8 | 7 |
| L6 | 54 | 25 | 11 / 9 / 5 / 0 / 0 | - | 10 | 0 |
| L7 | 64 | 28 | 10 / 8 / 3 / 7 / 0 | - | 13 | 16 |
| L8 | 86 | 39 | 12 / 11 / 4 / 7 / 5 | r52 = r41 (P_Dome_MF) | 15 | 28 |
| L9 | 96 | 46 | 12 / 10 / 9 / 14 / 1 | - | 32 | 26 |
| L10 | 109 | 47 | 10 / 12 / 10 / 11 / 4 | - | 31 | 23 |
| L11 | 105 | 53 | 12 / 11 / 5 / 17 / 8 | - | 27 | 27 |
| L12 | 115 | 62 | 27 / 18 / 15 / 1 / 1 | - | 31 | 2 |
| L13 (orig) | 104 | 50 | 22 / 8 / 10 / 1 / 9 | - | 21 | 11 |
| arena | 25 | 0 | - | - | 0 | 0 |

The copy counts plus the same-pose rows reproduce the diagnosis's
"later-sector rows re-using an object" column on every level (L1 23+1 = 24,
L3 17+1 = 18, L8 39+1 = 40, L11 53). The L1 case: rows 10 (s1, at
(-153.793, 16.5896, -272.917)), 37 (s3) and 38 (s4) all name
`P_Trafo_Wood_MF`, rows 37 and 38 at the same pose (302.035, -9.7479,
-830.547) (`audit2/ph1.txt:21, 48-49`). Row 37 gets the copy
`P_Trafo_Wood_MF_R37`, and row 38 must be rewritten to name that copy: left
on the original, the s4 pass would move the s1 trafo (§2.3 step 10). The
longest copied root name, suffix included, is 24 characters; the longest
child name seen (`P_Extra_Point_Ball_HitFrame06` plus a builder suffix and
`_R<row>`) stays under 40, below `MAX_NAME` 64 (`entity/session.hpp:65`).

L8 module and prop copies: P_Modul_30 rows 15, 48; P_Modul_25 rows 30, 64;
P_Modul_29 rows 50, 70; P_Modul_37 row 76; P_Modul_18 rows 36, 37, 54, 60;
P_Box rows 73, 74, 75, 80, 83. L11: P_Modul_25 rows 21, 23, 56, 76, 77;
P_Modul_37 rows 30, 48, 64; P_Modul_26 row 35; P_Modul_19 rows 43, 91;
P_Modul_34 row 55; P_Modul_01 rows 61, 75, 93; P_Modul_30 rows 87, 88;
P_Modul_18 rows 83-86, 89; P_Ball_Stone rows 39-42, 97-100.

## 2. The design

### 2.1 Overview

Three parts, all shared by every world:

- **(A) placement at the anchor**: a CK-free row plan (§2.2) and the CK step
  that executes it (§2.3), called once per world at its anchor (§2.4);
- **(B) the retail level lifetime** (§2.5): a retail level that ran a union
  is reloaded before the next session, and the copies are released at level
  exit, so every retail anchor builds from the same fresh level as the server;
- **(C) the union's level-walk gate** for #16 (§2.6).

None adds an engine change; IVP and physics_RT are untouched.

### 2.2 The row plan (CK-free)

New header `BallanceMMOCommon/include/session/placement_plan.hpp`
(header-only, no CK, compiled into the server, both clients and
`BallanceMMOMessageTests`):

```cpp
namespace bmmo::session {
    struct placement_row {            // one PH row as read at the anchor
        int row = 0, sector = 0;
        std::string type;             // column 1, Type Name
        std::string object;           // column 3's object name ("" = none)
        uint32_t matrix[16] = {};     // column 2, the 16 floats' bits
    };
    struct placement_action {
        enum class kind : uint8_t { keep, share, copy, none } what = kind::keep;
        int row = 0, sector = 0;
        std::string source;           // the object the row names now
        std::string name;             // copy: the new root's name; share: the holder's object
        int holder_row = -1;          // share: the row whose placement it shares
    };
    struct placement_plan {
        std::vector<placement_action> actions;   // one per row, in row order
        std::string error;                       // non-empty: fail closed
        size_t copies() const;
        size_t rewrites() const;                 // share rows whose holder object differs from `source`
    };
    struct entity_print {             // one 3D entity of a row's final hierarchy
        std::string name, mesh;       // mesh: the current mesh's name, "" if none
        int class_id = 0, scripts = 0;
        std::vector<std::string> attributes;     // attribute type NAMES, sorted
    };
    inline std::string placement_suffix(int row) { return "_R" + std::to_string(row); }
    placement_plan plan_placements(const std::vector<placement_row>& rows, size_t max_name);
    uint64_t placement_signature(const std::vector<placement_row>& final_rows,
                                 const std::vector<std::vector<entity_print>>& hierarchies);
}
```

**Algorithm.** Walk the rows in row order, keeping for each object name the
list of its placements (matrix bits, holder row, holder object name):

1. A row with no object: `none` (the manager's Set World Matrix on a null
   object is a no-op; nothing to copy).
2. The first row of an object: `keep`; its placement is the original's.
3. A later row whose matrix is bit-identical to one of the object's
   placements: `share` with the earliest such holder; `name` is the holder's
   object (the original, or an earlier row's copy). This is the user's
   "same-pose pairs stay pooled", including L1's pair between two later rows,
   where `name` (`P_Trafo_Wood_MF_R37`) differs from `source`
   (`P_Trafo_Wood_MF`) and the CK step rewrites the row (§2.3 step 10).
4. Otherwise `copy`: source = the row's object, name = source +
   `placement_suffix(row)`; the copy becomes a placement of that object.

Rows are the key, not sectors: the builder never gives one object two rows of
the same sector at different poses (Amount is the per-sector maximum), but the
plan does not rely on it. The `Set?` column is not read: it is a run-time
cell that `Reset PH-Array` rewrites (diagnosis §4 A), and a copy for a row the
manager never walks is inert.

**Names.** `_R<row>` keeps the `P_`/`PE_` prefix, so `is_mechanism_script`
(SR:195-202) selects a copy's script exactly as its original's, and a
`P_Extra` copy stays unselected. It contains no `_BMMO_`/`_Peer_` (the
mirror tags of PW:27, SC:1057, CH:1298), does not start with `Ball_`, uses
only `[A-Za-z0-9_]` (journal and harness tokenizers), and cannot collide with
a builder name (those end in three digits directly). The CK step applies the
same suffix to every copied 3D entity (§2.3 step 5), so every copy's name is
fixed by the plan and not by `GetObjectSecureName` (F1).

**Errors (fail closed: the host refuses to anchor).** One rule, the wire's:
a name longer than `MAX_NAME` (64) characters is an error
(`session_event_msg.hpp:88, 191` reject `size() > MAX_NAME`; 64 is allowed).
The plan checks it for the root names; the CK step checks it for every
renamed entity (§2.3 step 5), whose names are the body names that travel in
snapshots, events and the savestate. Two actions producing the same name is
an error.

**Idempotence.** Planning the table the CK step produced yields only `keep`
and `share` with `name == source` (every row names its own object or its
same-pose holder's). After §2.5 no retail anchor sees such a table in normal
operation; the property is kept as a unit test because it is what makes a
copy-holding table detectable (§2.4 "option off").

**Signature.** FNV-1a 64 over, in row order: row, sector, type, the final
object name, the 16 matrix words, and the fingerprint of that object's
hierarchy as it stands after the CK step: one `entity_print` per 3D entity
(name, class id, current mesh name, script count, attribute type **names**
sorted - type numbers are per process, F6), the entries sorted by name so
that a child order an engine keeps differently cannot change it. It is
computed from the final state, not from the actions, and it is computed in
every world whatever the session's options: with the placement off it is the
signature of the unplaced table. It names the world, so the server compares
it at SessionReady and every journal header carries it (§5).

The builder's dependency words (§2.3 step 2) are **logged, not hashed**: they
are level data, equal in every world, and what they change on a given engine
(scripts, attributes, groups, meshes) shows in the fingerprint or is set
explicitly by steps 7-8. A per-copy census of what the copy created (count of
created objects per class) is logged next to them (§2.3 step 12) and compared
in §7.5 run 1.

**Unit tests** (`BallanceMMOServer/tests/placement_plan_test.cpp`, new):
the L8 rows (types, names and exact matrix words captured once with the
`--list-placement` dump of §7.2 V1 and checked in) give 39 copies and row 52
sharing row 41 with `name == source`; the L1 rows give row 37 copying to
`P_Trafo_Wood_MF_R37` and row 38 sharing it with `name` `P_Trafo_Wood_MF_R37`,
`source` `P_Trafo_Wood_MF`, `rewrites() == 1`; the arena rows give 0; A(p),
B(q), C(p) makes C share A; A(p), B(q), C(q) makes C share B's copy; -0.0 vs
0.0 in one word is a copy (bitwise, stated); a 64-character result is
accepted and a 65-character one is an error; planning the applied table (the
L1 and L8 plans applied in the test's model: copies renamed, share rows
rewritten) is all keep/share with `rewrites() == 0`; the signature of an
applied table does not depend on the order of `entity_print`s given.

**As built (step S1, 2026-10-25).**

- **Where.** `BallanceMMOCommon/include/session/placement_plan.hpp` (the
  plan, CK-free, header-only, as specified above);
  `BallanceMMOCommon/include/game/sector_placement.hpp` (new: the read-only
  half of §2.3, see the adaptation below); SimTool `--list-placement`
  (`BallanceMMOServer/sim/placement_tool.hpp`, new; in `sim_tool.cpp` only a
  new `arguments` field at the end of the struct, one line in
  `parse_trafo_arg`, the new `run_list_placement` before
  `run_portable_scripts_check`'s comment block, one dispatch line in `main`
  and the usage
  lines); `BallanceMMOServer/tests/placement_plan_test.cpp` with
  `placement_plan_rows.inc` (the L1, L8 and arena PH tables, generated from
  the `--list-placement` listings: row, sector, type, object, the 16 words).
  Of the files trafo T3 also changes, none is touched (T3 does not change
  `sim_tool.cpp`).
- **Adaptation: `name` is filled for every kind.** keep: the source; copy:
  the new root; share: the holder's object; none: `""`. So `name` is what
  the row names after the CK step, and `placement_applied_rows(rows, plan)`
  (new, the test's and the listing's model of step 10) sets each row's object
  to it. Also new: `shares()`, `placement_kind_name`,
  `describe_placement_action` (the log line, `row 15 s2 P_Modul_30 copy
  P_Modul_30_MF -> P_Modul_30_MF_R15`).
- **Adaptation: the name rules.** The length rule (`> max_name` is an error,
  64 allowed) applies to the copies' root names, the names the plan makes;
  an original's name is the level's and is not the plan's to refuse. "Two
  actions producing the same name" is implemented as: a copy name equal to
  another copy's or to any object a row of the table names is an error (the
  latter is the only way a copy could meet an existing name the plan can
  see; the CK step's `GetObjectByNameAndParentClass` check of §2.3 step 5
  covers the rest of the world). On an error the actions are still filled for
  the log.
- **Adaptation: the signature's encoding**, which the prose leaves open:
  integers as 4 little-endian bytes, strings as a 4-byte length then the
  bytes, the row count and each hierarchy's print count first; the prints
  are sorted by name and then by every other field (two prints with one name
  cannot depend on the given order either); a row past the end of
  `hierarchies` hashes as an empty hierarchy.
- **Adaptation: §2.3 steps 1-2 landed here, read-only**, as free functions of
  `game/sector_placement.hpp`, so that S2 builds `sector_placement` on the
  same readers and `--list-placement` prints what the anchor will read:
  `read_placement_rows` (the five columns checked by name, array type and,
  for the two parameter columns, parameter GUID - matrix, bool - in the
  builder's order; the matrix read with `GetElementValue` into a `VxMatrix`
  and copied word for word; the Type Name read as the pointer the STRING
  cell stores, `GetElement(r, 1)`, whole and with no length cap, since it is
  hashed into the signature - `GetElementStringValue`'s 4-argument form is
  Ballanced's alone, the retail SDK has only the 3-argument one; and one
  name must mean one object: two rows naming different objects of one name
  are refused, since the plan keys objects by name - none of the 14 levels
  has such a pair), `find_builder_copy_blocks` (every Object Copy
  block of `Levelinit_build`, recursively, with its parent graph and the
  targets of its output 0; chosen = parent `Copy Objects` and a link to
  `TT Save IC`; an error unless exactly one is chosen, naming every block),
  `builder_copy_dependencies` (input 0 read as ObjectCopier reads it) and
  `describe_copy_dependencies`. Only calls both SDKs declare with one
  signature; nothing in the x86 mod includes it yet (S10 will), but an x86
  MSVC syntax probe of the header against the retail Virtools SDK compiles
  (and the same probe fails with C2660 on the 4-argument
  `GetElementStringValue`, the review's finding), and a GCC
  `-fsyntax-only` of it against Ballanced is clean.
- **`--list-placement`** builds the session world (`physics_world::create`,
  so the anchor is the server's: after the Sequencer restore, before any
  pass), prints the copy blocks, the block's settings and dependency words
  (per class with the bit meanings and the default copy words, then every
  nonzero word by class name), every PH row (`%a` matrix and the 16 words),
  one `plan` line per row, the copies by kind (extra / trafo / fan =
  P_Modul_18 / module / prop), the same-pose and rewritten rows, the longest
  copy name, the applied table's changed rows and its re-plan, and
  `placement: level N, R rows, C copies, S shared (W rewritten)`. Exit 0, or
  3 on a block, PH or plan error. The censuses and fingerprints of V1 come
  with S2's copies.
- **V1 (S1's part), all levels** (`--root C:/Users/geekerwan/Desktop/Ballance-Agent`;
  the original L13 from a scratch copy of the install with
  `Level_13.NMO.orig` as `Level_13.NMO`, md5 9b5be1ca6a92..., the arena is the
  install's `Level_13.NMO`): on all 14 the chosen block is `Copy
  Objects/Object Copy` (id 37186) -> `TT Save IC` and the one ignored block
  is `init and load Checkpoints/Object Copy` (id 37657) -> `fill
  Checkpoint-Arary`; rows, copies and kinds are exactly §1.4's table (L1 51
  rows 23 copies 10/2/0/0/11, L2 62/26 15/8/3/0/0, L3 53/17 8/8/0/1/0, L4
  75/29 16/11/1/0/1, L5 55/27 11/9/1/6/0, L6 54/25 11/9/5/0/0, L7 64/28
  10/8/3/7/0, L8 86/39 12/11/4/7/5, L9 96/46 12/10/9/14/1, L10 109/47
  10/12/10/11/4, L11 105/53 12/11/5/17/8, L12 115/62 27/18/15/1/1, L13 orig
  104/50 22/8/10/1/9, arena 25/0); the three same-pose pairs are bitwise
  equal and are the only shares (L1 r38 = r37, rewritten `P_Trafo_Wood_MF ->
  P_Trafo_Wood_MF_R37`; L3 r16 = r8 and L8 r52 = r41, `P_Dome_MF`, not
  rewritten); the longest copy name is 24 (L10-L12); the applied table
  re-plans to 0 copies and 0 rewrites on every level.
- **§9 Q1, settled from the printed words** (identical on all 14 levels):
  `m_Flags` 0 (custom, per class). OBJECT 0x3 (NAME and UNIQUENAME: the
  builder's `..001` names, F1), SCENEOBJECT 0x1 (SCENES), **BEOBJECT 0x7
  (scripts, attributes and groups are all copied)**, 3DENTITY 0x2 (CHILDREN
  only: meshes are not copied - F2 - and neither are animations), MESH 0x2,
  3DOBJECT, BEHAVIOR, MATERIAL and GROUP 0; the other nonzero words are
  2dEntity 3, Target Camera 1, 3D Sprite 2, Target Light 1, Character 1, Grid
  1, Layer 1, Array 2. The block is version 0x00010000 with Dynamic = 1:
  below 0x00020000 ObjectCopier ignores its Awake Object setting and always
  passes `CK_OBJECTCREATION_ACTIVATE` (ObjectCopier.cpp:132-141). What it
  means for S2: a copy made with these words arrives with the original's
  attributes and in the original's groups, in the hash order of F5, so the
  remove-then-append of §2.3 steps 8-9 is required, not defensive; UNIQUENAME
  is on, so step 5's rename is what fixes the names; S2's own copy passes no
  ACTIVATE and sets the state explicitly (step 6) - unchanged. The retail half
  of Q1 (the same words printed by the retail CK2, §7.5 run 1) stays with
  S10.
- **Checks.** `BallanceMMOMessageTests` 505/505 (11 new: the §2.2 cases and
  the L1, L8 and arena tables); the GCC syntax check, every line ok
  (`placement_plan_test.cpp` included); the x64 build; the wire check
  (`REF=core_ref_t3off`, `rTR2` vs `rSP1`): ALL SAME, 24 send traces and 24
  journals identical. Not run, because the change cannot reach them: the x86
  build (no retail file includes the new headers), the solo snapshots-off
  check (no rollback or session code changed) and every live run (no
  server, headless or retail code changed). Coverage: `linux_test_syntax.sh`
  and the Arch server build do not compile `game/sector_placement.hpp` or
  `placement_tool.hpp`; only the x64 SimTool build and the two probes above
  do. Review fix-up (the type reader, the one-name-one-object refusal, the
  test's limit taken from `MAX_NAME`, the MESH bits text): tests 505/505
  again, the GCC syntax check every line ok, the x64 build, and
  `--list-placement` re-run on all 14 levels: identical to the first
  listings except the MESH bits text (so every row's type and every plan
  line are unchanged by the new reader). No wire check: nothing but SimTool
  and the tests includes the changed headers.

### 2.3 The CK step (host support)

New header `BallanceMMOCommon/include/game/sector_placement.hpp`
(header-only, public CK2 API only, like `sector_activation.hpp`, so it
compiles against the Virtools SDK and Ballanced):

```cpp
namespace bmmo::game {
    class sector_placement {
    public:
        struct copy_record { CK_ID root = 0; std::string name; int row = 0; std::string source; };
        // Runs the plan on this world's PH table, all or nothing.  Empty
        // string: done (or nothing to do); otherwise why, the world is as it
        // was before the call, and it must not anchor.
        std::string apply(CKContext* context);
        // The signature of the PH table as it stands (§2.2); every host calls
        // it at every anchor, placement on or off.
        uint64_t signature(CKContext* context) const;
        // True if a PH row names an object whose name ends in _R<digits> or
        // this process's registry is not empty (§2.4, option off).
        bool holds_copies(CKContext* context) const;
        const std::vector<copy_record>& copies() const;     // made by THIS process
        std::string describe() const;                        // the anchor log lines
        // Puts each recorded row back on its source and destroys each
        // recorded root that still exists under its recorded name, with its
        // hierarchy and the scripts they own.  Returns how many it destroyed.
        int release(CKContext* context);
    };
}
```

`apply`, in this order:

1. **Read the PH table** (`CKDataArray "PH"`): check the five columns by name
   and type (Sector, Type Name, Matrix, Object, Set?) and refuse anything
   else; read each row into a `placement_row` (matrix by
   `GetElementValue(row, 2, &VxMatrix)`, words by `memcpy`). Plan it.
2. **The builder's dependencies.** Find the root script `Levelinit_build`.
   Walk it for blocks with the Object Copy prototype GUID
   `(0x3f6b0ac7, 0x47d20f78)` (ObjectCopier.cpp:28) and keep those whose
   parent graph is named `Copy Objects` and whose output 0 is linked to a
   block named `TT Save IC` (F4: id 37186 on the Ballanced load; ids are not
   compared, they differ between processes). Exactly one must match;
   otherwise refuse, naming every Object Copy block found with its parent
   graph. Read its input 0 the way the block does
   (`*(CKDependencies**)GetInputParameterReadDataPtr(0)`, ObjectCopier.cpp:130),
   which follows the link to `Copy Objects` local 1. Log the chosen block
   (`builder copy block: Copy Objects/Object Copy`), the ignored ones (the
   checkpoint copier in `init and load Checkpoints`), `m_Flags` and
   `GetClassDependencies(cid)` for CKCID_OBJECT, BEOBJECT, 3DENTITY,
   3DOBJECT, BEHAVIOR, MESH and GROUP.
3. **Refuse a hot source.** A `copy` source (or any entity in its hierarchy)
   that has a physics body, or any of its scripts' joint blocks
   (`script_replay::is_joint_block`) holding a handle, or a force block whose
   controller exists: refuse. A copy would carry the pointer, and the copy's
   first Create would delete the original's constraint (PhysicsHinge.cpp:
   152-158). At a server or headless anchor nothing is physicalized yet
   (`cores=0`, `movable=` empty in every anchor line); a retail anchor is on a
   freshly loaded level (§2.5) and prints `cores=0` today (the `anchored after
   0 load ticks: ... cores=0` lines of the retail logs).
4. **Copy, per `copy` action, in plan order.** Do what `CKContext::CopyObject`
   does (CK2/CKContext.cpp:97-107) with a context we keep:
   `CKDependenciesContext dc(context); dc.SetCreationMode(CK_OBJECTCREATION_DYNAMIC);
   dc.SetOperationMode(CK_DEPENDENCIES_COPY); dc.StartDependencies(builder_deps);
   dc.AddObjects(&source_id, 1); dc.Copy(nullptr);`. No
   `CK_OBJECTCREATION_ACTIVATE`: activation is set explicitly in step 6.
   A failed `Copy` already destroyed what it made (F5): refuse.
5. **Check and rename.** Let `made = dc.FillRemappedDependencies()` and `H`
   the source's hierarchy (`HierarchyParser` order, root first). Refuse if a
   member of `H` has no remap, if the root's remap is not a 3D entity, or if
   `made` holds a 3D entity that is not the remap of a member of `H` (a copy
   of an outside entity would be an unrenamed duplicate whose `UNIQUENAME`
   depends on the world's names, F1). Then, for every `o` in `H`:
   `c = dc.Remap(o)`, new name `o->GetName() + suffix`; refuse if it is longer
   than `MAX_NAME` or already present (`GetObjectByNameAndParentClass`); then
   `c->SetName(...)`. `dc.StopDependencies()`.
6. **Inert until placed, the way the builder's copies are.** The builder's
   copies are made at level load, when every original script is inactive, by
   a copy that activates BeObjects (F5), so they end up with their entities
   active in the level scene and their scripts inactive. Set exactly that,
   whatever the source's state: every copied 3D entity active in the level
   scene; every script of every copied BeObject `Activate(FALSE, FALSE)` with
   its ACTIVATENEXTFRAME/RESETNEXTFRAME flags cleared and
   `scene->DeActivate(script)`; the root hidden with its hierarchy
   (`Show(CKHIDE)`), which the pass's hierarchical Show undoes
   (`audit/d_Gameplay_SectorManager.txt:227, 358`). Nothing of the copy runs
   before its row's pass activates its MF script with reset.
7. **TT Save IC with hierarchy**, exactly as TT_Toolbox_RT SaveIC.cpp:67-93
   does: `scene->SetObjectInitialValue(e, CKSaveObjectState(e))` for the root
   and every hierarchy child, in the level scene.
8. **Attributes, in a fixed order.** For every `o` in `H` (HierarchyParser
   order) and every attribute of `o` (in `o`'s attribute order): if the copy
   already holds it (dependency bit 2), remove it; then set it with `o`'s
   value. Removing first makes the attribute managers' lists end with the
   copies in plan order, then hierarchy order (F7), whatever bit 2 did and in
   whatever hash order `Copy` made the objects (F5). This puts attribute 53
   on a trafo copy, so `create TrafoGroup` collects it whenever it runs (F6).
9. **Groups, in a fixed order.** For every group `G` that holds a member of
   `H` (the order in which groups are visited does not matter: each group's
   list is independent): remove every copy entity from `G` (dependency bit 4
   may have added them in hash order), then append the copy of each member of
   `H` that is in `G`, in HierarchyParser order. This covers the PH_Groups
   type group (the builder's own `Add To Group`), a `Trafos` group that exists
   at the anchor (F6), the sound and gameplay groups, and `All_Level` if the
   original is in it (§9 Q2).
10. **PH column 3.** For a `copy` row, `SetElementObject(row, 3, copy_root)`;
    for a `share` row whose `name` differs from its `source` (L1 row 38), the
    holder's copy root. Rows are written in row order after their action's
    copy exists.
11. **All or nothing.** Steps 3-10 refuse by returning the reason after
    undoing what this call did: every PH row written so far is put back on
    its source, and every object of every copy made so far (each copy's
    `made` list, kept until the end of `apply`) is destroyed with
    `CK_DESTROY_NONOTIFY`. Nothing of a refused placement stays in the world:
    no copy in a group, no attribute 53 holder for the next `create
    TrafoGroup`, no name to clash with the next attempt.
12. **Registry and log**: the copies go into the registry (`copies()`) only
    when the whole call succeeded. One line per copy (`row source -> name, n
    entities, census class:count..., groups, attributes`), the dependency
    words, the same-pose rows, the rewritten share rows, and `placement: N
    copies, M shared (K rewritten), signature %016llx`.

Order inside `apply`: the copies are made in plan (row) order, so their
creation order **across rows**, their append order in every group and their
order in every attribute list are the same in every world. Inside one copy
the creation order, and therefore the relative CK ids of a copy's objects,
follow the hash order of the source ids (F5) and are world-dependent; steps
8-9 remove that dependence from the lists, and §3.2 handles the ids.

**As built (step S2, 2026-10-25).**

- **Where.** `game/sector_placement.hpp`: after S1's readers, `placement_hooks`
  (the physics half of step 3, which the CK-only header cannot see: `has_body`,
  `force_exists`, and SimTool's `fail_at_row`), the helpers in
  `placement_detail` and `class sector_placement` with `apply`, `signature`,
  `holds_copies`, `copies`, `rows_written`, `describe` and `release`.
  `physics_world` (§2.4's server part): `world_options::placement_clones`
  (default off) and `placement_fail_at` (-1), `place_sectors()` (the hooks from
  `list_bodies` and `force_block_get`, the apply, the log, the signature), one
  call line in `anchor()`, `anchor_placement()` and `placement()`. SimTool:
  `--placement on|off` (default off; `create_session_world` and the `--nav`
  world pass it, `--replay` applies it at the record's first frame - built
  but not exercised: no retail tick record of a pooled level was at hand),
  `--placement-fail-at ROW`, and for the drop scenes `--tick-hashes` (the full
  world hash after every tick, V3) and `--watch-entity NAME` (positions and
  visibility on the report ticks: trafos, the player's ball); `--list-placement
  --placement on` lists the unplaced world as S1 does, then applies in the
  tool (`placement_tool.hpp list_placement_apply`). Of the files trafo T3 also
  changes: `physics_world.cpp` (the destructor's first line, the call line in
  `anchor()`, `place_sectors` at the end of the file), `physics_world.hpp` (an
  option block, two accessors, a member block) and `sim_tool.cpp` (an option
  block at the end of `arguments`, new functions, two lines each in
  `create_session_world`, the `--nav` world and `run_drop`, a block in
  `run_replay`, `run_list_placement`). `session_client.cpp`, `client_core*.cpp`,
  `rollback.hpp` and the savestate files are not touched; the headless client
  does not include the header.
- **Adaptation: the call site** is after the Sequencer **and** the proximity
  restore (step Q, PW `anchor()`), before the anchor hash: a copy inherits
  both level-file states from its source.
- **Adaptation: what a copy made** is every object created during its copy
  (the context's object ids before and after), not `FillRemappedDependencies`;
  both are printed and are equal for all 489 copies made on the 14 levels.
  The undo (step 11) destroys everything created since the first copy.
- **Adaptation: destroys notify.** The undo and `release` destroy with flags 0
  and empty dependencies, not `CK_DESTROY_NONOTIFY`: NONOTIFY skips every
  `CheckPreDeletion` (a group would keep the freed copy) and the managers'
  `OnSequenceToBeDeleted` (Ballanced `CKObjectManager::DeleteObjects`,
  CKObjectManager.cpp:249-262).
- **Adaptation: steps 8-9 act on the hierarchy's entities**, as written: a
  script is a `CKSceneObject`, not a `CKBeObject`, so no group or attribute
  holds one. Step 8 does not set an attribute type flagged
  `CK_ATTRIBUT_DONOTCOPY` (as `CKBeObject::Copy` does not; none occurs on the
  14 levels), and an object-valued attribute naming a member of the hierarchy
  names its copy. The attribute types the copies carry: Coll Detection ID,
  Continuous Contact ID, Particle Gravity and TrafoType (the trafo attribute of
  F6, on every trafo copy). Groups: each copy joins its type group
  (`All_P_<type>`), the boxes also `DepthTest`; `Trafos` does not exist yet at
  the anchor (Init Ingame builds it later from the TrafoType holders).
- **Adaptation: step 6's order.** Entities are activated only where they are
  not; each script is `DeActivate`d in the scene first, which sets
  DEACTIVATENEXTFRAME (CKScene.cpp `ActivateScript`), then `Activate(FALSE,
  FALSE)` and all three next-frame flags cleared; then `Show(CKHIDE)` on every
  hierarchy entity.
- **Adaptation: the length rule for renamed entities** is
  `min(MAX_NAME, BMMO_PHYSICS_NAME_SIZE - 1)` = 63, not 64: a renamed entity's
  name is its body's name, and the bridge's `bmmo_physics_body_state.name`
  holds 63 characters and a terminator. The longest renamed entity on the 14
  levels has 35 characters (L12); the longest copy root 24.
- **Addition: the placed table is checked.** After step 10 `apply` re-reads
  the PH table: every row must name its action's object and the table must
  re-plan to 0 copies and 0 rewrites, else it undoes.
- **Root cause found and fixed: a teardown crash of a placed world.** Symptom:
  `--level 11 --drop-sector 3 --drop-second-sector 1 --drop P_Ball_Stone 1
  --drop-at 694.546 -53.35 -227.162 --placement on` ran its 200 ticks and
  crashed in the world's destructor (0xC0000005 reading 0x00007FF600000008 in
  `ShowParticles`, TT_ParticleSystems_RT GeneralParticleSystem.cpp:110); the
  same scene with the placement off exits 0. Cause: the context reset
  (`CKObjectManager::OnCKReset`) destroys the dynamic objects in their
  creation order, which inside one copy is CK's hash order (F5), so a P_Extra
  copy's particle block got its BEHAVIORDETACH before its Extra block's. The
  particle DETACH deletes the emitter and calls `SetLocalParameterObject(0,
  NULL)`, which on x64 Ballanced copies the 8-byte pointer parameter's size
  from a 4-byte `CK_ID` (CKBehavior.cpp:1146-1155, `CKParameter::SetValue`
  memcpy of `m_Size`), leaving 0x00007FF600000000; the Extra block's DETACH
  then runs `DestroyExtra` (TT_Gravity_RT Extra.cpp:174-215, initialized by
  the s3 pass), which deactivates the children's particle scripts, whose
  DEACTIVATESCRIPT callback reads that pointer. Fix (no engine change):
  `release` destroys a copy's hierarchy scripts root first, then the rest, in
  one call (so every Extra deactivation runs while its emitters live), and
  every world releases its copies before its engine goes: the
  `physics_world` destructor and SimTool's `--replay` (S6 must do the same in
  the headless client). After the fix the scene exits 0 and its output is
  identical. The underlying x64 over-read is generic; 32 teardown scenes with
  builder copies (L2, L4, L6, L8-L12, sectors 2-6, placement off on rSP1 and
  on with this build) did not crash, retail x86 cannot meet it (4-byte
  pointers), and a fix in Ballanced's `SetLocalParameterObject` would be an
  engine change of its own. The builder's own copies (`*_MF001`) are made the
  same way, so an x64 server or headless world torn down after an Extra block
  ran could meet it at session end without any placement: queued as its own
  item (roadmap §4 item 6, "the x64 teardown over-read").
- **Checks (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`; tools and outputs in
  `.local-tools/work/SP2/`).**
  - **V1 complete**, all 14 levels (`v1.sh`): copies exactly as S1's plan and
    §1.4 (L1 23 with r38 rewritten to `P_Trafo_Wood_MF_R37`, L2 26, L3 17, L4
    29, L5 27, L6 25, L7 28, L8 39, L9 46, L10 47, L11 53, L12 62, original
    L13 50, arena 0 "nothing to copy"); every copy's census and fingerprint
    printed; every renamed entity <= 35 characters; the name review (§4) finds
    0 names that are not unique among the 3D entities across all PH rows'
    hierarchies (294-821 names per level); release then re-apply: inventory
    equal to the unplaced world's, `holds_copies` no, signature back to the
    unplaced one and the re-applied signature equal to the first: PASS on all
    14.
  - **V2**, L8 and L11, three runs each per arm: `anchored, hash=a8c7f832281a39c5`
    with surfaces 796eff6c40f81c4e (L8) and a34811ce6a4225ed (L11) identical on
    and off; signatures on 91a138e4fd6e7254 / 5d22d3b5a61c38cb, off
    629f6128b4fc8389 / 4f444149dc156465, stable over the three runs and equal
    to `--list-placement`'s.
  - **V3**: 601 per-tick full world hashes identical on vs off; the active
    root scripts identical; the s1 see-saw falls 0.048 m in both (the design's
    0.085 m is the diagnosis's older build; this build's own control gives
    0.048).
  - **V4** (every arm judged against its own control with the placement on,
    body by body on every report line, `cmp_parts.py`): L8 s1 plank with
    sector 2 at world ticks 0/30/50/70/150 SAME (off at 30: 0.265 m off the
    control); ball at the s2 site, sector 2 at 30 and 330 SAME (off: the plank
    woken at 330); L8 s2 rope bridge with sector 4 at 30 and 160 SAME (8.314 m
    sag, the control's; off: planks flung 1958 m); L10 P_Modul_41 with sector 4
    SAME (0 m); L11 P_Modul_37 with sector 2 SAME (0 m); L11 sack with sector 3
    at tick 160 SAME, Sack001 x 90.41..90.99 (off: x 84.68..96.75); L11 s1
    Pushers with a player at the s4 site and sector 4 SAME, never simulated (off:
    simulated on 11 report lines); L1 row 10 wood trafo with sector 4 SAME, it stays at
    (-153.7933, 16.5896, -272.9166) (off: moved 557.6 m to the s4 pose).
  - **V5**: a stone ball on L8 row 15's pose lands on `P_Modul_30_Wippe_R15`
    (stays at y 20.3-21.8; off: falls to -377); `P_Modul_29_Platte01..09_R50`
    and `P_Box_MF001_R73`, `_R74`, `_R75`, `_R80`, `_R83` have bodies; L11
    `P_Ball_Stone_MF001..004_R39..R42` and `P_Modul_01_*_R61` / `_R75` have
    bodies; the L10 s5 dome copy catches the ball at y -13.35 (off: the floor,
    -16.996). Adaptation: on L11 s4 the ball does not stay on the Pusher - the
    woken Pusher001_R61 pushes it off, exactly as the s1 original pushes a ball
    dropped on it (same run on row 0); the design's "rests on the Pusher" came
    from the diagnosis's control, whose module was never woken.
  - **V11**: the whole `--list-placement --placement on` listing (signatures,
    censuses, fingerprints, release checks) of the x64, the x86
    (`build/sim-x86-a3`) and the Linux (Arch) SimTool are identical on all 14
    levels, apart from the block ids and glibc's `%a` spelling of the matrices
    (the 16 words are identical).
  - **V12**, L8 row 50: refused at row 50 after 17 copies, 17 PH rows put
    back, 7464 objects destroyed, 0 left; PH table, 56 attribute lists (157
    holders), 54 group member lists (1386 members) and the names of all 55965
    objects equal to the world before the call; signature back to
    629f6128b4fc8389; `holds_copies` no: PASS. Through `physics_world` the same
    flag fails the anchor ("sector placement refused: row 50: ...").
  - The option is off by default: the wire check (`REF=core_ref_t3off`, rSP1
    vs rSP2) ALL SAME, 24 send traces and 24 journals identical; the reference
    solo server journal replays 2867/2867 matched, stdout identical to rSP1's.
- **Open, for later steps (found in the S2 reviews).** (a) The plan's name
  bound is looser than `apply`'s: `apply` and `--list-placement` call
  `plan_placements` with `session::MAX_NAME` (64) while step 5 refuses a
  renamed entity above 63, so a custom map with a 64-character copy root
  would pass the CK-free plan and fail the anchor; no level is close (longest
  root 24). S5, which puts the plan into the core's path, passes 63 to the
  plan. (b) The scene is only logged: `apply` prints "NOT the level scene"
  but step 7 then saves the initial values in whatever scene is current;
  every host today anchors in the level scene. S6/S10 (the host anchor calls)
  make it a refusal. (c) SimTool sends every `--placement*` argument to
  `parse_placement_arg`, which rejects any it does not know: S3's
  `--placement-trace` goes there.

### 2.4 Where it runs in each world

The rule (session-client-core-design §0): the decision - whether this
session builds the placement, the signature, what SessionReady and the
journal carry - is the core's; the CK work is host support; each host calls
it at one place in its anchor.

| world | call site | before | after |
|---|---|---|---|
| server | PW `physics_world::anchor`, between the Sequencer restore (PW:158-163) and `capture_world_hash` (PW:165) | the hash, `navigation_`, `rewire_proximity_probes` / `rewire_ball_identity_reads` (PW:239-240), the checkpoint hold | `level_sequencers_.restore` |
| headless | SC `anchor()`, between the Sequencer restore (SC:1442-1448) and `capture_world_hash` (SC:1451) | `session_script_selection` (SC:1474), `explosion_pieces`, `reset_guard_.resolve`, the checkpoint hold | `level_sequencers_.restore` |
| retail core | CH `core_host_anchor`, between `level_sequencers_.restore` (CH:859-863) and `physics_view_.capture` (CH:865) | `session_script_selection` (CH:878), `explosion_pieces` (CH:879) | N1-N4 restore and resolve (CH:846-858), the Sequencer restore |
| retail legacy | none: a SessionStart whose `world_options` has `WORLD_PLACEMENT_CLONES` ends the legacy session locally with "the placement needs the session core (session core on)" before the restart. No other change: no feature is added to the legacy path, which R7 deletes (`docs/session-client-core-design.md:4040-4057`). | | |
| SimTool | every mode that creates a `physics_world` gets it through PW; `--replay` (record mode) runs it at the record's first frame when `--placement on`; `--replay-session` follows the journal header (§5) | | |

Why each constraint:

- **After the Sequencer restore**: the copies inherit the level file's
  counters from their restored originals.
- **Before any pass and any body**: the sector-1 walk starts two frames after
  the anchor (F9), so no joint handle and no module body exists yet; step 3
  enforces it everywhere.
- **Before the server's rewiring** (PW:239-240): `rewire_proximity_probes`
  and `rewire_ball_identity_reads` find their blocks by structure in every
  behaviour, so the copies' probes and ball reads get their own per-player
  rewiring (L8 today rewires 29 probes and 3 reads, `audit2/l8_PH_Groups.txt`).
- **Before the selection** (SC:1474, CH:878): the copies' scripts are in the
  selection from the first record; a selection taken earlier would miss them
  and every rewind would leave them live.
- **Before the hash**: the anchor hashes do not see the copies (no body), but
  computing them after the world is complete keeps the anchor line honest.

**Option off.** Every host computes `signature()` at every anchor. With
`WORLD_PLACEMENT_CLONES` clear and `holds_copies()` true (a retail level whose
reload of §2.5 did not happen, or a registry `release` missed), the host calls
`release` first and anchors only if `holds_copies()` is then false; otherwise
it ends locally with "the level still holds placement copies (row N names
X)". The signature it reports is then the unplaced table's, which is the
server's; a table it could not restore cannot reach SessionReady.

**The core's part.** `session_start_msg` gains `world_options` (§5); the
core keeps it (`world_options()`), the host runs `apply` when
`PLACEMENT_CLONES` is set and reports `signature()` in `anchor_report`
(client_core.hpp:650-657 gains `uint64_t placement`) in every case;
`session_client_core::anchor` (client_core.cpp:222-262) puts it in
SessionReady and the journal header. A host whose `apply` failed does not
call `anchor` and ends locally with the reason (the same path as a failed
hash, CH:865-868).

**The server's part.** `physics_world` takes the option (default **off** in
the struct, so nothing before S5 builds copies by accident) and exposes
`anchor_placement()`; `world_ready_info` (SRN hpp:119-131) gains `uint64_t
anchor_placement`, filled where the runner fills `anchor_hash`
(SRN cpp:252-253), and passed to `open_journal` with it (SRN cpp:229); `on_world_ready` (SV:966-980) stores it in the session next to
`anchor_hash`; `handle_session_ready` compares it right after the engine check
(SV:1551-1569) and before the anchor-hash compare (SV:1570), **without** that
compare's `!s.late.count(c)` exemption (§5).

### 2.5 Retail: the level a session builds from

The server and the headless client load a fresh level for every world. The
retail client anchors on an in-level restart (`restart_current_level` on
"Go!", CH:660-668; the anchor is the first frame `Gameplay_Ingame` is active
again, CH:712-716), so its level outlives sessions. Two consequences:

1. **A union leaves the level dirty (the old §9 Q3).** The vanilla restart
   deactivates the current sector only; the first session's other union
   sectors (originals and, with this design, copies) stay physicalized and
   their scripts keep their state. A second session on that level would
   anchor on a world the server does not have. This is pre-existing (it does
   not need the placement) and no retail log has ever measured it (F10: no
   log holds two anchors in one load), so step S9 measures it first.
2. **Copies outlive the session.** The PH rows keep naming the copies.

**Rule: every retail anchor is on a level loaded after the last union on
it.** The host keeps a file-scope flag `level_ran_union` (the mod may not add
members to `BallanceMMOClient`), set at the anchor, cleared on a map load
(`OnLoadObject(isMap)`, MC:233). On a SessionStart that will restart (the
countdown, CH:538-668), a set flag makes the host reload the level the way
the mod's automation does (F15; the function moves from CA into host support
so both use one implementation) **at the start of the countdown**, and "Go!"
restarts only once the load has finished and the Sequencer capture window
after a map load has passed (MC:349-357), with the restart deadline counted
from then. The anchor is then exactly today's measured path - fresh load,
then restart - on which every retail anchor so far printed `cores=0` and the
server's hashes. A clean level (vanilla play only since its load) restarts as
today: vanilla tears down per sector, so it holds no union state and no
copies.

Why reload, not tear down (the alternative was: at session end, with the
guard off and N2 restored, run one vanilla deactivation pass per union
sector): a teardown pass is a scripted state change whose completeness has to
be proven for every module type (hinge shutdown, restore IC, the force
controllers, the proximity probes, the sleep states), on the very path the
user's rule (a) says must match the server, which never tears down. A reload
needs no proof: the server builds the same fresh level. The cost is a level
load per session after a union, overlapped with the 3-second countdown; S9
measures it against the start barrier (`kStartBarrierTimeout` 8 s, SRN
hpp:139).

- **Sequencer counters.** Since every anchor follows a load, the 30-frame
  capture after the map load (MC:349-357) records the originals, the anchor's
  `restore` resets them, and the copies made after it inherit the restored
  values. No capture of the copies is needed.
- **Level exit.** Whether the game destroys dynamic copies at exit is not
  known (§9 Q2). The host makes it irrelevant: `OnPostExitLevel` (MC:360),
  after Event_handler's `Exit Level` has dephysicalized every entity of the
  scene, calls `placement.release(context)`, which puts each recorded row
  back on its source and destroys each recorded root whose object still
  exists **under the recorded name** (an id alone may have been reused, F13),
  with its hierarchy children and the scripts they own (`CKContext::
  DestroyObject` with a `CKDependencies` that deletes 3D-entity children and
  BeObject scripts). The registry is a file-scope static in CH, cleared after
  the release. The reload above passes through the same exit.
- **Orphans.** `apply` refuses when a planned name already exists (§2.3 step
  5) and leaves the world as it was (step 11). After a crash or a missed
  release that could block a session on the next load of the same level; the
  refusal names the object, and the release on the next map load
  (`OnLoadObject(isMap)`, MC:233) removes recorded leftovers before any
  anchor.
- **After the session.** Vanilla play on the dirty level until the next
  session or load is outside every session (no shared state). What the
  player sees there (earlier union sectors still physicalized) is §9 Q3.

**As built (step SP9 = S9, 2026-10-25).** Retail only: `BallanceMMOClient/session/core_host.cpp`,
`BallanceMMOClient/BallanceMMOClient.cpp` (two hook lines), `BallanceMMOClient/BallanceMMOClient.h`
(four non-virtual methods, no data member) and `BallanceMMOClient/automation/client_automation.cpp`
(the `session reload` verb routed). Nothing under `BallanceMMOCommon`, the headless client or the
server changed; of the files trafo T3 also changes, none is touched. Runs (two retail clients, L11,
session core on, the Arch server of HEAD 81c6803, snapshots off) under `scripts/retail/sp9_*`; run
script `.local-tools/work/SP9/sp9_retail.py`.

- **(1) The gap, measured.** Session 1 on a menu-loaded L11, client 1 crossing s1 -> s3 (`sector 2`,
  `sector 3`: union `running: 1 2 3` on both), then the host restarts the session (`room start
  physics` again) with no reload. Session 1 anchors `a8c7f832281a39c5 / a34811ce6a4225ed cores=0`
  (the server's); session 2 anchors client 1 `560bb584d6758db9 / d08289d97445702b cores=2`, client 2
  `5f19f8f3de4657c0 / d08289d97445702b cores=2`, and the server ends it: `world mismatch for
  Player673_0 (client 5f19.../d082..., server a8c7.../a348...)` (`sp9_gap_1`, the HEAD binary).
  Repeated with this step's binary and `session reload off` (`sp9_gap_2`: `28b968edf184f7cc` /
  `38591266aa0a8508`, same surfaces, same mismatch), whose new `anchor bodies:` line gives the body
  list at the anchor itself: 61 bodies against the fresh anchor's 48, all 48 identical, 13 extra -
  sector 1's mechanisms (`P_Ball_Stone_MF`, `_MF005`, both `P_Modul_01` rows, `P_Modul_25_Bridge001`,
  `P_Modul_26_Rope` and `_Sack` still swinging, flag 1: the two awake cores and the per-client pose
  hash) and sector 2's `P_Modul_19_Flaps001` and `P_Modul_30_Wippe001`; none of sector 3's; the script
  replay selection `45 scripts (10 active)` against a fresh anchor's `(3 active)`. **Cause:** the
  anchor's vanilla restart (Event_handler `reset Level`: `Set Cell IngameParameter[0][2]`, then
  `Execute Script Gameplay_SectorManager`) deactivates the sector in the cell only, which the union
  left at 3; sectors 1 and 2, which the union never deactivates, stay physicalized with their scripts
  and their session-1 state. (Membership from session 1's journal: sector 1's bodies appear at ticks
  14-16, sector 2's at the pass at 134 - `Flaps`, `Flaps001`, `Wippe001`, `Kiste001`,
  `Schiebestein001` - sector 3's at 270; the pooled `Flaps`, `Kiste001`, `Schiebestein001` are sector
  3's rows by then and went with it.) This is the expected result (§2.5 item 1). The P_Modul_26
  swing is the same cause: sector 1 was never reset.
- **(2) Built.** File-scope `level_lifetime` in CH: `ran_union` (this design's `level_ran_union`) set
  at every core anchor and in every frame a legacy session runs (read in `core_host_process`, so the
  legacy path itself is unchanged), cleared by `core_host_map_loaded()` from `OnLoadObject(isMap)`
  (MC). At a core SessionStart that passes the in-level check, a set flag starts the reload at the
  start of the countdown and logs `the level ran a union: reloading level N`; a clear one logs `a
  clean level ...: the restart needs no load` and restarts as before. "Go!" (`core_host_go()`, the
  restart factored out of the countdown) runs only when `core_host_reload_ready()`: the map load
  seen since the exit, `level_sequencers_frames_` at 0 (the capture window), the menus' request done,
  in game, `Gameplay_Ingame` active (no pause test: the restart opens the pause menu itself, as the
  clean path's "Go!" does from a paused level); a "Go!" that comes first waits (`"Go!" waits for the
  reload`) and the countdown's frame check restarts when ready; the restart deadline (20 s) counts
  from then. The reloaded level is the session's (`msg.map.level`, what the server simulates); a
  session map that is not an original level slot (1..13) ends the session locally instead of loading
  something else. The reload's own deadline is 10 s (`the level reload did not finish`): the server's
  start barrier (8 s from its SessionStart) is the effective limit - past it the server ends the
  session for this member - and the host's give-up is the barrier plus a margin, so its log names its
  own cause when no SessionEnd comes. A failed menu request ends the session too. `OnPostExitLevel` (MC) skips `physics_session_end_local` once for the
  reload's own exit (`core_host_reload_exit()`, true only while a core countdown reloads). No reload
  at session end (§11, Q3).
- **Adaptation: how the level is reloaded.** The design moves the automation's direct load (F15:
  `CurrentLevel[0,0]`, `Load Level` to Event_handler's owner) into host support. Measured from inside
  L11 (`level 11 direct`): Event_handler fires PreLoadLevel and nothing loads, the level runs on
  (no OnLoadObject, `level_time` still counting) - its `Load Level` branch only runs from the menu.
  So the host sends what the pause menu's "Exit Level" button sends (Menu_Pause: `Exit Level` to
  `Level` and to `All_Balls`, without its Yes/No) and starts the level through the menus with the
  existing `level_request_` (`game/menu_driver.hpp`, already the one level start shared by the
  automation's `level N`, the headless engine and SimTool); `automation_load_level` is unchanged and
  is not called by the host. The one implementation is therefore the menu driver, not the CA function.
- **Additions.** `anchor bodies [i/n]:` (the anchor's full body list, as the server's `world: anchor
  bodies:` line, which the server cuts at 900 characters) at every core anchor, logged in pieces of at
  most 900 characters so no body is cut and no single log line grows with the level. Testing tools:
  `session reload on|off|delay <ms>|status` - `off` reproduces the gap (1), `delay` makes the
  reloaded level ready only that long after its map load (the "Go!"-waits path).
- **Gate (2).** `sp9_two_1`: three sessions in one game run, each with s1 -> s3 crossings; session 1
  clean (no load), sessions 2 and 3 reload (exit seen 0.83 s, map load 1.50 s, ready 2.12 s after the
  exit, before "Go!") and anchor `a8c7f832281a39c5 / a34811ce6a4225ed cores=0` on both clients, the
  server's; SessionStart -> anchor 3.75 s (the barrier is 8 s); the three anchors' body lists (48)
  identical on both clients and with the session-1 fresh anchor, and the server's logged 19 bodies
  identical. `sp9_goslow_2` (`delay 2500`): "Go!" waited 1.0 s, anchor 4.74 s after SessionStart, the
  server's hashes, `cores=0`. `sp9_paused_1`: both players in the pause menu at the restart: the
  reload works, the server's hashes, no menu left active. Three launches of both copies answered
  within 1.8 s; the legacy path (`session core off`) anchors on the server's hashes with no reload.
  After the review fix-ups (no pause test, the session's level, the 10 s give-up, the pieced body
  line): `sp9_two_fix`, three sessions with s1 -> s3 crossings: session 1 clean, sessions 2 and 3
  reload (map load 1.52-1.54 s, ready 2.14-2.17 s after the exit, before "Go!"), every anchor
  `a8c7f832281a39c5 / a34811ce6a4225ed cores=0` on both clients and the server, SessionStart ->
  anchor 3.75 s, the six rejoined body lists (48, three pieces each) identical, no mismatch.
- **Build-tree finding (pre-existing, not code).** The first runs of HEAD's x86 mod crashed or
  stalled every client at the first core SessionStart (one run: both processes gone after
  `countdown started`; three probes: `session core` showed `sessions=1 made=0 error=''`, the core
  never made and no log line): the incremental build had recompiled the client-core objects but not
  `core_host.cpp.obj` (the tree was last built from other sources, most likely trafo T3's parked
  tree), so the host and the core disagreed on a layout. Deleting the `BallanceMMOClient.dir` objects and rebuilding cured it; the S10 builds
  must do the same after any header change (the rule in the R notes).
- **Not covered: Level 1.** A reloaded L1 starts its tutorial (the reload's load runs `OnStartLevel`
  with `countdown_restart_` false, so the mod's tutorial exit is not fired); if the tutorial holds
  `Gameplay_Ingame` inactive the reload never becomes ready and the server's barrier ends the session
  (no hang). L1 is excluded from this step's gate until the queued Level-1-tutorial-off step, which
  owns it; the gate ran on L11 only.

### 2.6 The union's level-walk gate (#16)

**Symptom** (audit §5 #16, reproduced here, F9). A late joiner's union holds
sectors owed from `sector_history`; its pacer starts idle and reads the
manager's state before each step; the level's own sector-1 activation is
issued by Init Ingame two frames after the anchor, so the owed pass starts
at relative tick 2, in the frame the level's own activation arrives, and
writes `IngameParameter[0][1]` first. Sector 1 is never walked in that world
(L8 SimTool: only the s2 Wippe exists; networked `lp1_join` client3:
mechanisms 52/45). The union, which holds sector 1 as active from the
anchor, never re-issues it.

**Cause.** SU:82-104 lets a pass start once the pacer has seen three idle
ticks, and nothing tells it that the level's own walk has not started yet.

**Rule.** No union pass may start before the level's own walk has been seen,
and "seen" is an edge: the manager observed **inactive** at some tick after
the anchor and **active** at a later one. An edge cannot be produced by a
walk already running at the anchor (a retail restart's teardown, if one ever
overlapped the anchor; F10 found none in 197 retail anchors), only by a walk
that starts after it.

```cpp
// sector_union::tick, after applying the stamps and counting the pacer:
const bool may_start = pacer_.tick(manager_active);
if (!level_walk_seen_) {
    if (!manager_active) level_walk_armed_ = true;
    else if (level_walk_armed_) { level_walk_seen_ = true; level_walk_tick_ = tick; }
    if (!level_walk_seen_ && ++level_walk_wait_ > kLevelWalkWait) {
        level_walk_seen_ = true; level_walk_tick_ = tick; ++level_walk_timeouts_;
    }
    if (!level_walk_seen_ || manager_active) return {};
}
```

`kLevelWalkWait` = 64 ticks. Every host logs `level walk seen at tick N` (or
`level walk timeout at tick N`), and the status line counts timeouts. The
timeout cannot reintroduce #16: by tick 64 a walk that started after the
anchor has ended (a walk spans about seven frames, SP:8-12), and the pacer
still needs three idle ticks. `reset()` (SU:118) clears the gate. The rule
lives in the pure struct, so the server (`update_sectors`, PW:788), the
headless and retail cores (`sector_union_pass`, client_core_frame.cpp:188-198)
and the legacy retail path get it at once.

**Effect.** Members of a session (requested == active at the start) are
unaffected unless a stamp is due before the walk ends, in which case every
world delays it identically - provided every world sees the edge at the same
tick, which §7.4 and §7.5 check through the logged N (server, headless and
retail equal). A joiner walks sector 1, then its owed sectors in ascending
order, one per idle manager. With (A), which objects those passes place no
longer depends on the order.

**Unit tests** (`rollback_engine_test`'s union cases or a new
`sector_union_test.cpp`): owed {2} at tick 0, manager inactive 0-2, active
3-6, inactive from 7: the gate opens at tick 3 and the pass for 2 starts at
tick 9 (three idle ticks after the walk), not at 2; manager active 0-4 (a walk
running at the anchor), inactive 5-6, active 7-10, inactive from 11: the gate
opens at 7, not at 0, and the pass starts at 13; active 0-20 then inactive
for ever: the timeout opens it at tick 64, the pass starts at 64, the timeout
counter is 1 (64: `wait` passes `kLevelWalkWait` at tick 64 counting from
tick 0 - corrected at step S4, this read 65); never active: the same; requested == active: no pass and no
change to the pacer's counts; `reset()` re-arms the gate.

**As built (step S4, 2026-10-24; built inside the resync track's step A5,
which needs it).**

- **Where.** `session/sector_union.hpp`: the gate is `gated_sector_union`, a
  class derived from `sector_union` (`tick` = the base's `tick_gated` with
  its gate; `state`/`restore` carry it; `walk()`, `walk_open()`,
  `walk_timeouts()`; `reset()` closes it). Every world of the session core
  (`client_core_state::sectors_`), the re-derivation (`sector_rederivation`)
  and the server's world (`physics_world::sectors_` and its step state) run
  it. Log lines: the core `level walk seen at tick N` (or `timeout`) in
  `sector_union_pass`, the server `world: level walk seen at tick N` in
  `update_sectors`; the status line's `sectors:` group gains
  `walk_timeouts=`.
- **Adaptation: the legacy retail path keeps the gate-free union.** This
  section says the legacy path gets the rule at once. It cannot without a
  layout change: `physics_session_state` holds a `sector_union` by value
  (physics_session.hpp:291) and keeps its layout until R7
  (session-client-core design 3.2), and the legacy path gets no feature.
  So `sector_union` keeps its members exactly (the gate's state lives in the
  derived class) and the legacy path behaves as before: a legacy late joiner
  keeps audit #16, and a stamp due before the level's walk ends (none in any
  recipe: the earliest stamp is hundreds of ticks in) would start there on
  the legacy path a few ticks before the server.
- **Adaptation: the gate's state is {seen, armed, wait}**, normalized to
  {true, false, 0} when it opens; where and how it opened (`walk_open()`:
  the tick, by the timeout or not, and `known`: whether that tick is one of
  the current numbering) is the live union's diagnostic, like the counters,
  so a state round-trips exactly. A renumbering (`forget_walk_tick()`, called
  by both of the core's renumbering assignments) clears `known`: the tick
  names the old numbering. Step A5's level-walk drop requires it (the
  resync design's As built (step A5)).
- **Adaptation: the bytes stay protocol 10's.** The resync state's union
  bytes (`serialize`, version 1) carry no gate: a state with the gate open
  reads back open, and a state with it closed is one the format cannot hold
  (serialized to nothing, refused on reading). The one state that travels,
  the server's at S = base - 1, is past the server's own walk for a late
  joiner (it loads the level after the runner started). An A3 resync whose
  S lies before the server's walk (a request in the session's first ticks)
  gets no union bytes (the server logs `the sector union does not
  serialize`) and the client keeps its anchor's own union: neither union can
  have started a pass before its walk, so the re-step decides the same
  passes.
- **The timeout tick.** With the code above run from tick 0, `wait` passes
  64 at tick 64, so the timeout opens the gate (and the pass starts) at tick
  64, not 65 as the prose said; the prose above is corrected and the unit
  test asserts 64.
- **Checks.**
  - Unit tests (`SectorUnion.ThePassWaitsForTheLevelsOwnWalk`: the five
    cases above with the 64 of the previous item, the gate-free union's pass
    at tick 2 unchanged, the gate as rollback state and closed-gate bytes
    refused); every other union, core and re-derivation test opens the gate
    first (`open_gate`), as after the level's walk. 466/466 with A5's.
  - V6 (SimTool, L8, `--savestate-check --drop-sector 1
    --drop-second-sector 2 --drop-settle 60`): `world: level walk seen at
    tick 3`, `world: sector +2 at tick 9`, objects 65 at t0 = 43 of sector 1
    alone + 22 of sector 2 (rA4: `sector +2 at tick 2`, 64); `--drop-sector 2
    --drop-second-sector 3`: walk at 3, `+2 at 9`, `+3 at 16`, 73 objects,
    50 savestate rounds failed 0. `P_Modul_30_Wippe_R15` needs S2's copies
    and is not checkable before S2.
  - Arena: `level walk seen at tick 30` (base 27 + 3) on the server and
    every client in every live run of A5, and in all 24 replays of the
    `core_ref_r3` captures, whose sends and journals are unchanged
    (`wire_check.sh rA4 rA5`: ALL SAME); `sector +N` ticks as before
    (lane/resync recipe 824/1270/1713-1785, the join recipe 825/1270/1714 as
    in `lp1_join`); `walk_timeouts=0` everywhere. `lp1_join` client3 52/52:
    see the resync design's As built (step A5).

### 2.7 Interplay with what already exists

| mechanism | interaction |
|---|---|
| N1 death and level reset (`sector_reset_guard.hpp`, both Gameplay_Ingame call sites) | none: the guard empties Execute Script targets; the copies are PH rows like any other. A client-only level reset runs Init Ingame again: `create TrafoGroup` rebuilds `Trafos` from attribute-53 holders in list order, the copies included (step 8). |
| N2 deactivate pins, `Reset PH-Array` | none: column 4 only; option A's trap (re-asserting Set? before every pass) does not exist for B. |
| N3 checkpoint hold, N4 phase cell | none: they edit Gameplay_Events and the checkpoint scripts, not PH objects. The checkpoint copier (F4, 37657) is not touched. |
| body guard (#6, #13) | `KeepsBodyOf` is by id and covers every level body (CKIpionManager.h:336-339), so a copy's body is kept on clients exactly as an original's. The guard's short-circuit snap (Physicalize.cpp:113-114) no longer meets a re-placed module. |
| `rewire_*` (server) | the copies are rewired (§2.4). |
| Trafo Manager | finds trafo copies in `Trafos` (F6, steps 8-9). |
| shared outside objects (FixCube, Ball_Pos_Frame, CurrentLevel, IngameParameter) | stay shared; FixCube couples no units (F11); step 5 refuses a copy that would duplicate one. |
| world and surface hashes | the anchor's pose and surface hashes are unchanged (no copy has a body at the anchor; checked in §7.2 V2); they therefore cannot tell a placed world from an unplaced one, which is why the signature exists. TICK hashes change as soon as a copy's bodies exist. |
| hull cache (#9) | a shared Collision Surface name keeps compiling its hull from the first body physicalized with it (§3.4). |
| P_Modul_01 | each copy physicalizes once at its own row's pass (phase true) and is never unphysicalized under the union (F14); nothing moves its frame. |

## 3. Determinism

### 3.1 Same inputs, same placement

- **Input.** The PH table at the anchor: built at level load by
  Levelinit_build from the level file, in an order the builder's iterators
  fix; read and walked identically by the server and the clients today (the
  union's passes are bit-exact, lockstep doc "The sector union, as built").
  The plan is a pure function of it. On retail the table is a fresh load's
  (§2.5), like the server's.
- **The copy.** `CopyObjects` reproduces the source's state at the anchor.
  That state is already required to be equal in every world - the source is
  an original or a builder copy the sector passes activate today, so its
  scripts' locals, matrices and Sequencer counters must agree at the anchor
  or the originals themselves would diverge (design 9.26 is the case where
  they did not, and its restore runs first). What a copy must not inherit -
  pointers (step 3), activation (step 6) - is set explicitly; names are set
  from the plan (step 5); attributes and groups are set from the original in
  a fixed order whatever the dependency flags and the hash order did (steps
  8-9).
- **The checks.** The signature folds in every row's object hierarchy
  (names, classes, meshes, script counts, attribute type names); every world
  reports it and SessionReady refuses a member or joiner whose signature is
  not the server's (§5), so a retail CK2 whose `CopyObjects` builds a
  different hierarchy cannot join silently. The census (§2.3 step 12) shows
  what a mismatch is made of.

### 3.2 What still depends on CK ids

The CK_IDs of copies and originals differ between worlds (F13), and inside
one copy their relative order follows the hash order of the source ids (F5).
A retail process whose earlier teardowns freed ids can give a copy an id
**below** its original's, where a fresh server gives it one above. Three
things read ids:

1. **The script replay's order** (`select` sorts by priority then id,
   SR:230-268; `execute_frame` runs that order, SR:482-513). It is in-world
   state (records never leave the process), so cross-world it does not
   matter. Within a world it must equal the live order for a replayed frame
   in which two selected scripts make bodies; see 3.
2. **Group and attribute iteration** (`Get Nearest In Group`, `Objects With
   Attribute Iterator`): their order is the list order, which steps 8-9 make
   the same in every world (F7). Before its pass, a trafo copy sits at its
   source's anchor pose, hidden, so all copies of one source coincide there
   and `Get Nearest In Group` has an exact tie whenever the ball is nearest to
   that pose. The tie is resolved by list order, which is the same in every
   world; vanilla's teardown (TT Restore IC) creates the same coincidence
   with the builder's copies today.
3. **The live behaviour order**: `CKBehaviorManager::ManageObjectsActivity`
   adds next-frame objects in hash order of their ids and re-sorts with the
   (deterministic, unstable) qsort by priority (CK2/CKBehaviorManager.cpp:
   88-108, 131-133), so two equal-priority modules activated in the same
   frame can run in a different order in two worlds. This exists today for
   originals and builder copies whenever a retail client's object history
   differs from the server's; copies add more such pairs in one pass (L8 s2
   walks copies and originals together), and copies below their originals
   reverse both the (priority, id) replay order and the next-frame hash
   order of a copy/original pair. It changes physics only if the two scripts'
   body creations interact (IVP list order). §7.2 V7 measures it with ids
   perturbed both ways (copies above and below the originals); §9 Q5 has the
   options if it is not neutral.

### 3.3 Late joiners, resyncs, out-of-order passes

- **Late joiners** load the level fresh (a retail joiner on a dirty level
  reloads it, §2.5) and run the same anchor: the same copies and the same
  signature, which the server compares for joiners too. With the gate (§2.6)
  they walk sector 1, then the owed sectors; each pass touches its own rows'
  objects only, so the order of the owed passes does not change the
  placement.
- **Resyncs** rebuild nothing: the copies exist, the snapshot names their
  bodies (`P_Modul_30_Wippe_R15` etc.), the union is not reset.
- **Out-of-order passes** (the automation `sector N` verb, a cell write): the
  placement no longer depends on pass order, which removes the diagnosis's
  pass-order dependence (§5 "Pass-order dependence"). What remains
  order-dependent is which copy of a shared surface physicalizes first
  (§3.4) and the birth order of a shared same-pose Type 2 body (the domes:
  identical pose and geometry either way; §9 Q9).

### 3.4 The hull cache

`CreatePhysicsObjectOnParameters` caches a convex hull per Collision Surface
name, compiled from the first entity physicalized with it and its scale
(engine changes #9, #10). Copies share the name (F2). For a module part the
scale comes from the part's local matrix under its MF, which every copy has
from the original; for a Type 2 root (P_Box) it comes from the row's own
matrix, whose rows may be one ulp off unit length, so the first-physicalized
box decides the hull of all boxes. Lockstep worlds physicalize in the same
order. A joiner after out-of-order passes, or a retail process whose cache
was filled by earlier play, may compile it from another row. This is the
same exposure as the builder's pooled boxes today; §7.2 V8 measures whether
any shared surface's hull differs between rows; §9 Q6 has the options.

## 4. Rollback interplay

- **Body plan.** A copy's bodies are born in its own sector's pass, like any
  module's: tracked under their unique names (`movable_bodies`, the mechanism
  registry), recorded, snapshotted. Nothing moves a live module any more, so
  the diagnosis's R1 (a window across T_act replays on the post-activation
  hinge) and R2 (the replayed activation rebuilds the hinge from a replayed
  pose) cannot occur; the joint-presence check (SR restore) and the
  unrecorded frame matrices stop mattering for this class.
- **Window-born births.** The existing path (lockstep doc "The sector
  activation, replayed"): the body is destroyed at the restore, its entity put
  back at its Physicalize matrix (`put_back_birth`, HS:390-403, local matrix
  under the copy's MF), and the replayed manager places the copy and its
  script makes the body again. Names are the keys of every step:
  `put_back_birth` and `destroy_body` look entities up with
  `GetObjectByNameAndParentClass` (first match), `local_birth_log` and the
  recipe lines key by name, `joints_on` (SR:158-175) matches Target/Object2 by
  name. All need unique names, which step 5 guarantees for every copied
  entity (it refuses a clash).
- **Savestate.** Bodies are keyed by name; controller names are
  `rt:force/<behavior id>` (engine change #18), and each copy has its own
  blocks. Mesh and surface names in the recipe resolve to the shared meshes
  (F2), as they do for the builder's copies.
- **Selection.** Copies of P_ modules are selected (SR:195-202), P_Extra
  copies are not; the copies of `P_Modul_18_Particle` are selected like the
  original (visual only, §9 Q7). A restore writes every selected script
  including inactive ones (lockstep doc), so the records grow with the copies
  from the anchor on: on L8 about 15 scripts, 219 blocks and 2,700
  parameters (the module scripts' block and parameter counts in
  `audit/d_*Script*.txt`), on top of an estimated 4,000-5,000 parameters
  today; the largest growth is L9 (+32 scripts), L10 (+31), L12 (+31) and L11
  (+27) (§1.4). §7.6 measures each.
- **Records.** No record format changes: the copies are ordinary selected
  scripts and tracked bodies; the record version, the script-replay version
  (2) and the savestate version stay.
- **Manager activations.** `manager_activations` (HS:202-310) re-issues the
  pass; the replayed walk places copies with the same matrices.
- **For F3 (resync with script state, later):** the server cannot ship
  script records by selection index across worlds (ids and therefore order
  differ, §3.2), and script names are not unique (copies keep them); key the
  records by `script@owner`, whose owner names are unique.

## 5. Wire, protocol and journal

| item | change |
|---|---|
| `PHYSICS_PROTOCOL_VERSION` (entity/session.hpp) | 11 -> 12 (written 9 -> 10 before resync A3 took 10 and trafo T2 took 11, which also brought `world_options` into SessionStart), comment: "12: the world option WORLD_PLACEMENT_CLONES; SessionReady carries the placement signature (docs/sector-placement-design.md)". Forward compatibility deliberately broken. |
| `entity/session.hpp` | `constexpr uint32_t WORLD_PLACEMENT_CLONES = 1u << 0;` |
| `session_start_msg` | `uint32_t world_options` after `protocol` (exists since protocol 11, trafo T2); the server sets bit 0 from config `physics.placement_clones` (default **false** from S5 to S10: adaptation M3, see As built (step S5) below; **true** since S10, As built (step SP10)). It is the switch for the negative control (§7.6) and for replaying old journals; it is never a per-client choice. |
| `session_ready_msg` | `uint64_t anchor_placement` appended after `caps`, read when `protocol >= 12`; filled by a host that declares `CAP_PLACEMENT` (placement on or off, §2.4), 0 otherwise (As built (step S5)). |
| `world_ready_info`, `on_world_ready` (SRN hpp:119-131, SV:966-980) | `anchor_placement` from `physics_world`, stored in the session. |
| server `handle_session_ready` (SV:1537-1577) | after the engine check, before the anchor-hash compare: `anchor_placement != s.anchor_placement` ends the session with "placement mismatch for X (client %016llx, server %016llx)", **for late joiners too** (unlike the anchor hashes, the signature does not depend on time), the way the protocol and engine mismatches already do for any client. |
| `journal_header` (journal.hpp) | `uint64_t anchor_placement` trailing after `world_options` (which trafo T2 appended at v5); `kJournalVersion` 5 -> 6; an older file reads as placement 0 (unknown, not compared), a file older than v5 as options 0 (no copies). |
| SimTool `--replay-session` | builds the world with the header's options and, for a header with a non-zero placement, refuses a replay whose anchor placement differs from the header's ("journal placement X, replay Y"). |
| legacy retail path | parses the new fields (shared message code) and ends locally on `WORLD_PLACEMENT_CLONES` (§2.4). |
| `docs/rooms-and-sessions-protocol.md` | the two fields. |

Until S5 nothing builds copies outside SimTool: `physics_world`'s option
defaults to off and the server has no config for it, so the protocol-9 arena
runs of S4 are unaffected (the arena has 0 copies either way).

**As built (step SP5 = S5, 2026-10-27).**

- **Numbers.** `PHYSICS_PROTOCOL_VERSION` 11 -> **12**, `kJournalVersion` 5
  -> **6** (the next free ones after resync A3's 10 / v4 and trafo T2's 11 /
  v5; the table above said 9 -> 10 and 3 -> 4 and is corrected).
  `WORLD_PLACEMENT_CLONES = 1u << 0` in the `world_options` field trafo T2
  put into SessionStart (a u32 after `protocol`; nothing added to the
  message). `CAP_PLACEMENT = 1u << 3` (new, below).
- **Adaptation M3 (the orchestrator's): the config default is false.** The
  server's `physics.placement_clones` (`config_manager.hpp`) defaults to
  **false** until S10, not "true from S5": the retail core host computes no
  signature and builds no copy before S10, so with the option on every
  retail session on L1-L13 would refuse the retail members (and with a
  signature compare against 0, end with "placement mismatch"). As T2 kept
  `trafo_inputs` off until its hosts ran it. Set, it gives every session
  bit 0 at `start_physics_session`, logged ("every PH row gets its own
  object"), and the runner builds the world with `placement_clones`.
- **Adaptation: a capability, `CAP_PLACEMENT`, instead of "always filled".**
  §2.4 has every host report its signature at every anchor, but the
  hosts gain the signature at S6 (headless) and S10 (retail core), and the
  legacy path never does (no feature is added to it). By §0 (a host that
  cannot says so with a capability, the core takes the fallback):
  `session_capabilities::placement` (`client_platform.hpp`); with it the core
  sends `CAP_PLACEMENT` and `anchor_report::placement` in SessionReady's new
  `anchor_placement` (and the journal header), without it 0 and no cap.
  The server (`handle_session_ready`, right after the engine check, before
  the trafo refusal and the anchor-hash compare): a member **with** the cap
  whose signature differs ends the session with "placement mismatch for X
  (client %016llx, server %016llx)", late joiners included (no `late`
  exemption); a member **without** it is not compared, and in a session with
  `WORLD_PLACEMENT_CLONES` it is refused (SessionEnd to it alone, out of the
  session, not the room), as T2 refuses a member without `CAP_TRAFO_INPUTS`.
  This also covers the legacy path's §2.4 row from the server's side until
  S10 adds its local end. Once S6 and S10 declare the cap, every session-core
  member is compared at every anchor, which is §2.4's rule.
- **Adaptation: the header field trails.** `anchor_placement` is the v6
  header's last field (after T2's trailing `world_options`), not "after
  `anchor_surfaces`"; an older file reads 0 = unknown, not compared.
- **Fix: the client journal header carries `world_options`.** T2 wrote the
  world options into the server's header only; a client header said 0, so
  a `--replay-session` of a client journal would build its world without
  the session's options - with the placement it would then be refused by
  the signature compare. `client_journal::open` now writes `s.world_options_`
  and the signature SessionReady carried (0 without the cap). A client
  journal of a trafo-on session therefore replays with the trafo inputs
  from now on; that a client journal holds every Trafo event such a replay
  needs (the server's own `TRAFO_SERVER` resets included) is not checked
  here (deferred, below).
- **The headless test switch.** `--placement-signature HEX`
  (`session_client.cpp`): the host declares the capability and reports HEX
  (it computes no signature before S6, which replaces the switch's base with
  the real signature). For a session with the option off only; it builds no
  copy. Nothing enforces "option off" (the server lets a member with the
  cap and a matching signature in whatever the host built); S6 replaces the
  switch with the real signature.
- **SimTool.** `--replay-session` builds the world with the header's
  `WORLD_PLACEMENT_CLONES` and refuses (exit 1) a journal whose non-zero
  `anchor_placement` differs from the replay world's ("journal placement X,
  replay Y: ... (refused)"); `--write-journal` writes the replay world's
  signature. `--replay` (record mode) applies the placement at the record's
  first frame **by default** ("record default on"; `--placement off` for a
  record made without the copies, i.e. every retail record of a pooled level
  before S10 and every single-player record whenever made: only a session
  world with the option holds copies), and prints which.
  `scripts/determinism_baseline.py` (the `mech_L*.bmrc` single-player
  corpus) therefore passes `--placement off` in its three replay command
  lines (review finding of SP5). `--world-options PLACEMENT_CLONES` uses the
  constant now.
- **S2's open bullet (a), the plan bound.** `game::kPlacementMaxName` =
  min(MAX_NAME, BMMO_PHYSICS_NAME_SIZE - 1) = 63 is passed to every
  `plan_placements` call (`apply`'s plan and its re-plan, `--list-placement`
  and its re-plan; the listing's line reads "(bound 63)"), so a 64-character
  copy root is refused by the plan, not at step 5.
- **Where.** `entity/session.hpp` (protocol, `CAP_PLACEMENT`,
  `WORLD_PLACEMENT_CLONES`), `message/session_ready_msg.hpp`
  (`anchor_placement`, read from protocol 12), `session/journal.hpp` (v6),
  `session/client_platform.hpp` (`placement`), `session/client_core.hpp`
  (`anchor_report::placement`, `world_options()`), `src/session/client_core.cpp`
  (begin's log line, anchor's caps, signature and log line),
  `session/client_journal.hpp/.cpp`, `game/sector_placement.hpp`
  (`kPlacementMaxName`), `session/placement_plan.hpp` (comment); server
  `config_manager.*`, `server.cpp` (session field, start, `on_world_ready`,
  the compare), `sim/session_runner.*` (`world_ready_info::anchor_placement`,
  the world option, the journal header), `sim/sim_tool.cpp`,
  `sim/placement_tool.hpp`, `sim/session_client.cpp` (the switch);
  `scripts/journal_trace.py` reads v6. The retail mod is untouched in
  source: its core host declares no `placement` (S10), the legacy path
  sends 0 without the cap (it sets `caps = 0`) and writes a v6 header with
  0.
- **Checks (quick tier, 2026-10-27, about 8 minutes of wall clock; tools and
  outputs in `.local-tools/work/SP5/`).** x64 BUILD-OK, x86
  CLIENT-BUILD-OK (`BallanceMMOClient` objects deleted first: the
  capability and message structs changed), Arch ARCH-BUILD-OK
  (`ballanced-3e4289e95f25+bmmo-168e53eb52d3-dirty`); 530/530 tests
  (`PlacementWire` x4, `SessionJournal.V6AnchorPlacement`,
  `SessionClientCorePlacement` x2, `PlacementPlan.TheHostsBoundIs63`); GCC 29
  ok. **A v5 arena journal still replays matched:**
  `core_ref_rc1/resync_snapoff/server.bmjr` (protocol 11, 4 clients) with
  rSP5: 4112/4112 matched, 2056 checkpoints, 0 mismatches, stdout identical
  to rTR5's. **§7.4 "mismatch"** (one live run, `maps/arena-test/sp5_mismatch`,
  snapshots off, option off, `work/SP5/mismatch_run.sh`): client 0 with
  `--placement-signature ecfbfd9b63a330b5` (the server's arena signature)
  started and ran (assigned, tick 409+, its log "anchor placement signature
  ecfbfd9b63a330b5"); client 1 joining late at tick 600 with signature 1 ended
  the session: "placement mismatch for Arena1 (client 0000000000000001, server
  ecfbfd9b63a330b5)" on the server and both clients. The run's v6 server
  journal (header `anchor_placement=ecfbfd9b63a330b5`, the client's the same)
  replays 608/608 matched; the same file with the header's signature set to
  1 (`work/SP5/tamper.py`) is refused ("journal placement 0000000000000001,
  replay ecfbfd9b63a330b5"); the rTR5 SimTool refuses the v6 file ("journal
  version 6 (expected 2..5)"). **The protocol-11 captures refuse**, as
  expected: `core_ref_rc1/resync_snapoff` capture 0 replayed by rSP5 logs
  "the server runs physics protocol 11, this client runs 12" and stops; no
  wire check is possible until S6 re-captures the set (`core_ref_rc1`,
  `core_ref_t3`, `core_ref_t3off` are all protocol 11). Of "mismatch" only
  the late joiner ran live; a start member's mismatch takes the same branch
  of `handle_session_ready` (no `late` condition on it) but no test or run
  covers it - S6's run starts its mismatching client at session start.
- **Deferred** (roadmap §4 item 7's list): the wire check (needs S6's
  re-capture); a retail two-copy run with core off and on (the SessionReady
  bytes changed for both modes; no retail source changed); a session with
  `placement_clones` on (its refusal of a member without the cap, and of
  the retail core host); `--replay` record mode with the new default (no
  retail tick record of a pooled level at hand, as at S2); a
  `--replay-session` of a client journal from a trafo-on session (its header
  now carries `world_options`).

## 6. What must not change

- **The arena and every level with nothing pooled**: zero copies, the same
  selection, the same anchor and TICK hashes: an arena journal recorded before
  the change replays `matched` all ticks, and §5.2-style A/B arena runs are
  identical.
- **A session that stays in sector 1 of an original level**: the copies
  exist but have no body and run nothing, so sector 1's per-tick physics is
  bit-identical with the option on and off (§7.2 V3).
- **The same-pose pairs** (L1 rows 37/38, L3 rows 8/16, L8 rows 41/52) stay
  pooled; L1 row 38 follows row 37 onto its copy.
- **The builder's own copies** (`*_MF001`...) and the checkpoint copies are
  untouched and keep their names.
- **N1-N4, the union's no-deactivate rule, the pacer's one pass per idle
  manager** (the gate only delays the first pass of a world until its level
  walk); the death and level reset chains.
- **No engine change, no IVP change**; the record, script-replay and
  savestate formats.
- **The retail anchor path itself**: fresh load, restart, first active frame
  of Gameplay_Ingame; §2.5 only adds the load in front when a union dirtied
  the level.
- **Exception (c), the level-end balloon and its airship**: `PE_Levelende`
  has one row per level and is never copied; the design does not touch its
  local status. If a custom map pooled it, each placement would get its own
  copy like any other row, still local.

## 7. Verification

### 7.1 Unit tests (`BallanceMMOMessageTests`)

`placement_plan_test` (§2.2), the union gate cases (§2.6), message round
trips of `session_start_msg` / `session_ready_msg` at protocol 12 and 11
(11 parses with placement 0), journal header v6 write/read and v5 read. Pass:
all green on x64 MSVC, x86 MSVC and GCC.

### 7.2 SimTool checks (offline, `--root C:/Users/geekerwan/Desktop/Ballance-Agent`)

New SimTool options: `--placement on|off` (default off until S5, then on in
every record mode; replays follow the header), `--list-placement` (prints the
chosen and ignored Object Copy blocks, the dependency words, the plan, every
copy's census and fingerprint and the PH rows with `%a` matrices, then exits),
`--second-move SECTOR TICK` (repeatable: the parked second player of
`--drop-second-sector` reports these sectors at these ticks),
`--placement-trace PATH` (§7.3), `--perturb-ids N` (creates and destroys N
dummy objects after the level load, before the anchor: copies get ids above
the originals'), `--free-ids-before-load N` (creates N dummies before the
level load and destroys them with `CK_DESTROY_FREEID` in ascending id order,
so the level and then the copies draw ids from the free list top-down and the
copies get ids **below** the originals', F13), and `--savestate-check`
accepting `--drop-move`/`--second-move` inside the window (today refused,
ST:1712-1716; it needs `manager_activations` recorded and re-issued like the
clients do).

| id | check | pass |
|---|---|---|
| V1 | `--list-placement` on L1-L12, original L13, arena | chosen block `Copy Objects/Object Copy` and one ignored block (`init and load Checkpoints`) on every original level; copies and shares exactly as in §1.4's table; L1 row 38 names `P_Trafo_Wood_MF_R37` in the applied table; the three same-pose pairs bitwise equal; every renamed entity's name <= 64; dependency words and censuses printed |
| V2 | anchor lines, `--placement on` vs `off`, L8 and L11 | `anchored, hash=... surfaces=...` identical; signatures differ (on vs off) and each is stable over three runs |
| V3 | L8 drop on the s1 see-saw (`--drop P_Modul_30 1 --drop-at 75.202 8.5 0.031`), sector 1 only, 600 ticks, on vs off | per-tick world hash identical; fell 0.085 m |
| V4 | the diagnosis table re-run with `on` | L8 s1 plank 0.085 m with move 2 at ticks 0/30/50/70/150 and 0.000 m with the ball at the s2 site; L8 s2 bridge equal to its sector-4-free control (8.6 m / 2.697 m in the diagnosis's two setups) with sector 4 up; L10 P_Modul_41 0 m with sector 4; L11 P_Modul_37 0 m with sector 2; the L11 sack swings in z only with sector 3; L11 s1 Pushers not woken by a player at the s4 site; L1 s1 wood trafo (row 10) does not move when sector 4 activates |
| V5 | later instances exist | a ball dropped on L8 row 15's pose lands on `P_Modul_30_Wippe_R15`; L8 s4 bridge (`P_Modul_29_*_R50`) and rows 73-75 boxes physicalize; L11 s3 stone balls (rows 39-42) and s4 P_Modul_01 (rows 61, 75) have bodies and the ball rests on the Pusher; L10 P_Dome at s5 catches the ball at -13.33 |
| V6 | late joiner, L8 `--drop-second-sector 2` (and `3` with player 1 in 2) | `level walk seen at tick 3`; `sector +2` no earlier than the third idle tick after the walk (tick 9); `P_Modul_25_Bridge001` and the s1 Wippe exist, `P_Modul_30_Wippe_R15` exists at s2 |
| V7 | id independence: L8 and L11 drop scenes with sectors 2-5 (L11: 2-6) activated by `--second-move`, three arms: `--perturb-ids 37`, `--perturb-ids 1000`, `--free-ids-before-load 20000` (enough that the copies come from the free list; the arm prints one copy/original id pair to show the copy is below), each against `--perturb-ids 0` | per-tick world hash identical through all passes in every arm |
| V8 | shared hulls: `--dump-surfaces-at` after all passes, pass orders 2,3,4,5 vs 5,4,3,2 (L8) | every shared surface's hull identical |
| V9 | `--savestate-check --frames` on L8 with `--second-move 2 T` inside the window, rounds 50, depth 48; once more with `--free-ids-before-load 20000` | PASS (every chained restore reproduces the straight run's blobs) |
| V10 | the negative control of §7.3 with `--placement off` | FAIL, naming the s1 Wippe |
| V11 | platforms: V1's signature and V7's hashes from the x64, x86 (`build-sim-x86`) and Linux (WSL) SimTool builds | identical |
| V12 | all-or-nothing: a SimTool test hook `--placement-fail-at ROW` that makes step 5 refuse at that row (L8, row 50) | `apply` returns the reason; afterwards the PH table, the `Trafos`-relevant attribute list, every group's member list and the set of object names equal those of `--placement off` (printed and compared by the tool) |

### 7.3 The invariant check (I1)

`scripts/placement_check.py LEVEL` runs SimTool twice per arm with
`--placement-trace`, which writes, after every tick, one line per object
hierarchy of the **watched sector's** PH rows: for every body its full core
state (position, both quaternions, speeds, speed changes, movement state,
time of last PSI: the world hash's `core_state`, unrounded); for every joint
block of its scripts the handle **value** (a rebuilt constraint is a new
pointer) and its referential's world matrix; for every force block
`force_block_get`; for every TT Scaleable Proximity block its ObjectB world
matrix and its locals; and every entity's world matrix (trafos, extras, fan
frames included). The control arm activates only the watched sector; the
test arm then lets the second player enter the later sectors at the given
ticks. PASS iff the two traces are byte-identical.

| arm | watched | later passes (`--second-move`) | ball |
|---|---|---|---|
| L8-a | s1 | 2 @30, 3 @50, 4 @70 (plank awake) | stone on the s1 see-saw from tick 0 |
| L8-b | s1 | 2 @150, 3 @170, 4 @190 (plank asleep), second drop on the plank at 300 | as a, plus the second drop |
| L8-c | s1 | 2 @0 | ball at the s2 site only (the remote-wake case) |
| L8-d | s3 | 4 @60 (the shared dome r41/r52) | ball resting on the dome |
| L1-a | s1 | 3 @30, 4 @60 (the r37/r38 trafo pair) | ball near the s1 wood trafo (row 10), not on it |
| L11-a | s1 | 2 @30, 3 @60, 4 @90 | on the P_Modul_25 bridge |
| L11-b | s1 | 3 @130 | the sack swing (s1 P_Modul_26_MF001 row 9) |
| L11-c | s2 | 3 @60, 4 @90 | on the s2 P_Modul_37 |

Pass: every arm identical for 900 ticks; V10 (the same arms with
`--placement off`) fails at the first later pass (L1-a: the s1 trafo moves at
the s4 pass). An arm that fails with placement on is a coupling between
sectors that is not pooling (a global IVP or CK state): report it with the
first differing field, symptom, cause and options; do not waive it.

**As built (step S3, 2026-10-25).**

- **Where.** `BallanceMMOServer/sim/placement_trace.hpp` (new: the trace
  writer); SimTool (`sim_tool.cpp`): an option block at the end of
  `arguments`, three branches in `parse_placement_arg` and two names in the
  line that routes to it, new functions after `settle_world` (`scene_trace`,
  `scene_tick`, `settle_scene`, `open_scene_trace`, `close_scene_trace`,
  `check_scene_switches`), one changed line in `setup_sector_scene` (its
  settle runs through `settle_scene`), three in `run_drop` (open the trace,
  tick through `scene_tick`, close), one check line in `main` (before every
  mode's dispatch, so `--replay-session` with a trace is refused, not
  silently traceless, until S8 builds it) and the usage lines;
  `scripts/placement_check.py` (new). Of the files trafo T3 also
  changes, none is touched (T3 does not change `sim_tool.cpp`); nothing under
  `BallanceMMOCommon`, the headless client or the server changed.
- **The switches.** `--second-move SECTOR TICK` (repeatable) needs
  `--drop-second-sector`; `--placement-trace PATH` traces the `--drop-sector`
  rows (a relative path is resolved against the working directory at parse
  time, before the engine makes the game directory the working directory);
  all S3 switches are refused outside the plain `--drop` scene
  (`--savestate-check` takes a pass inside its window at S7, `--replay-session`
  the networked trace at S8). Addition: `--drop-again X Y Z TICK`
  (repeatable) beams the player's ball there at rest before that tick, for
  L8-b's second drop.
- **Adaptation: ticks are world ticks** (the world's tick index; the drop
  scene's settle, 30 ticks, counts, so the ball is dropped before world tick
  30 and `2 @0` is a pass during the settle). The table's ticks are used as
  written in that unit; L11-b's `3 @130` is therefore 30 ticks earlier than
  V4's sack run (`--drop-move 3 130` = world tick 160), still inside the
  swing.
- **Adaptation: both arms have the second player**, parked in the watched
  sector (`--drop-second-sector` = the watched sector), so the arms differ by
  the `--second-move` passes alone.
- **Adaptation: a joint block's handle is traced as present or not, not by
  value.** Cause: the heap does not give two processes the same addresses
  (Windows' low-fragmentation heap randomizes them), so whether a re-created
  constraint reuses the freed address differs from run to run. Measured: three
  runs of one command (L11-c's control, `--placement off`, 130 ticks) gave
  three different handle sequences (one run kept two handles at reused
  addresses for 123 lines, the others did not), and a first version that
  traced the value as a first-seen ordinal failed V10's L11-c at tick 14,
  before its first pass, on `P_Modul_19_MF001`'s hinge alone (ordinal 2 vs 8).
  A rebuild from a moved frame shows in the referential's matrix (traced), and
  any rebuild that changes the constraint shows in the bodies' states. Blind
  spot left: a test-arm-only rebuild from the same frame, with the same
  constraint, while the bodies sleep, shows nowhere until they wake (L8-b's
  second drop is the arm that wakes them); the script activity below narrows
  it to a rebuild by a script that is active in both arms.
- **Addition: every script's activity** (`a=1/0` per script of the
  hierarchy): a script a later pass deactivates (the principle that entering
  the next sector changes nothing in the previous one) or a joint script that
  runs again in one arm shows even while the bodies sleep.
- **Addition: player 1's ball** (`--placement-trace-ball`, one line per tick
  after the rows: the entity and its body as above). The ball is the watched
  bodies' contact partner, which the PH rows do not hold, so it is what shows
  §9 Q9's "and contacts". The script traces it on every arm except L8-c,
  where the ball sits on the s2 copy, which only the test arm places (a
  legitimate difference).
- **Addition: every body, asleep or not.** `describe_cores_exact` lists only
  the manager's moving objects, so a sleeping body's core is absent from it;
  each entity therefore carries `list_bodies`' description of its body
  (movable, simulated, collision, movement state, pose, speeds, `%a`; "-" for
  none) and, while the core is moving, the full core state.
- **Additions to the script**: `--repeat` (each arm's control twice: the
  trace's own reproducibility) and, on a failure, every differing hierarchy
  with its first differing tick and the entities and blocks that differ. An
  arm is an ERROR, not a PASS, when its runs did not do what it asks: a
  test-arm pass not reported or its sector never activated (`world: sector
  +S`), the control activating any sector but the watched one, a
  `--drop-again` not done, a `sector event failed` or `tick failed`, or an
  empty trace (0 lines or 0 hierarchies: a missing PH array or a sector with
  no rows). V10 also requires the row the arm names (`v10_row`: row 0
  `P_Modul_30_MF`, the s1 Wippe, for L8-a/b/c) among the differing rows.
- **Scenes** (`--drop-at`; the drop height 4 m is added): L8-a/b/c the
  s1 see-saw (75.202, 8.5, 0.031), L8-c with `--drop-player-at` the s2 site
  (402.572, 21.815, -58.067), L8-d the dome r41 (637.863, 13.52, -337.955), L1-a
  10 m off the s1 trafo (-163.793, 16.59, -272.917), L11-a the s1 bridge row 14
  (115.885, -26.41, -75.984), L11-b the sack (90.729, -23.725, -50.767), L11-c the
  s2 P_Modul_37 row 30 (429.639, -46.793, -83.437). The plank is awake at the
  L8-a passes (movement state 1 at 30/50/70) and asleep at L8-b's (8 at
  150-190); L8-b's second drop wakes it (asleep at 310, awake at 400).
- **Results** (`.local-tools/work/SP3/fix/check_on.log`, `check_off.log`, the
  review round's final runs; 930 world ticks per run; the 16 runs of a mode
  take 5 s together).
  - **I1, placement on: all eight arms PASS**, traces byte-identical, every
    pass reported and activated, the control activating nothing else: L8-a
    and b 11160 lines (11 hierarchies and the ball line; 2 movable bodies
    besides the ball, as in every count here, 2
    joints, 1 force block, 9 proximities, 33 scripts), L8-c 10230 (no ball
    line), L8-d 13950 (14 hierarchies, 8 movable and 1 fixed body, 8 joints,
    8 force blocks, 12 proximities, 29 scripts), L1-a 15810, L11-a and b 16740
    (17 hierarchies, 15 movable and 4 fixed bodies, 7 joints, 4 force blocks,
    8 proximities, 28 scripts), L11-c 15810 (sector 2, 7 joints, 15
    proximities, 37 scripts). `--repeat`: every control reproduces, on and
    off.
  - **V10, placement off: all eight arms FAIL, each after its first later
    pass**: L8-a at tick 34 (pass 30) on row 0 `P_Modul_30_MF`: the root, the
    HingeFrame, the proximity and the hinge (the s1 Wippe); L8-b at 154 (pass
    150), the Wippe's body as well; L8-c at 13 (the s2 pass starts at 9 under
    the union gate); L8-d at 64 (pass 60) on the s3 trafo, fan and extras
    rows; the dome r41 and the ball do not differ in either mode over the
    900 ticks, and the ball is on the dome's crown at the pass (y 15.21 at
    world tick 59, 0.07 m off its centre, rolling slowly; it rolls off to
    y 11.5 by tick 330): §9 Q9's re-placement of the same-pose dome is a no-op
    for its body and its contact here; L1-a at 34 (pass 30) on row 0 `P_Extra_Life_MF` and
    row 10 `P_Trafo_Wood_MF` - the s1 trafo moves at the **s3** pass already,
    since with the placement off row 37 (s3) names the same object as rows 10
    and 38; L11-a at 34 on the s1 modules (P_Modul_37, both P_Modul_25), later
    the stone balls, the sack and both P_Modul_01; L11-b at 134 (pass 130); L11-c
    at 64 (pass 60) on the s2 P_Modul_19, P_Modul_25, P_Modul_34, P_Modul_37.
- **Limit found: Level 1's physics is frozen in the session world.** On L1
  the full world hash (environment clock included) is constant over 201 ticks
  of the drop scene and the dropped ball never moves. Measured cause: the
  physics time factor is 0 at the end of the L1-a run (`get_clock`: time
  factor 0, physics delta 0; 0.002 and 0.030303 on L8 and L11), the
  tutorial's pause (the known item "switch the Level 1 tutorial off",
  roadmap). So L1-a
  checks the CK side of I1 there (the trafo pair r37/r38, the extras, the
  proximities) and its bodies only at rest; its physics side waits for that
  item. In L8-c the ball wakes the s2 copy (`P_Modul_30_Wippe_R15` moves
  0.33 m) and then rolls off it (y -602 at the end), which is the remote-wake
  case the arm is for.

### 7.4 Headless harness arms

New headless options: `--sector-at SECTOR:TICK` (repeatable; through
`core.request_sector` at that tick, like `--start-sector`) and a repeatable
`--beam`. Recipes at `--latency-ms 0` and `150`, snapshot interval 2, the
current deployed lead and input delay:

| arm | clients | pass criteria |
|---|---|---|
| L8 cross | c0 stays in s1 (beamed onto the see-saw), c1 s1->s2, c2 s1->s2->s3, c3 s1->s4 | every client and the server log the same `placement` signature, the same `level walk seen at tick N` and the same `sector +N at tick T`; `mechanisms=K/K` on every client with K including the copies; LOCAL rows for `P_Modul_30_Wippe_R15/_R48`; server journal replays matched; `first_divergence.py` clean on s1 and s2 bodies at 0 ms; at 150 ms the mechanism and prop residuals no worse than the same arm's control (placement off) per body that exists in both |
| L8 invariant (networked) | the server journal of "L8 cross" replayed twice by SimTool: as recorded, and with the Sector events of sectors > 1 dropped (new `--replay-session --drop-sectors-above 1`) | the s1 rows' trace (§7.3 format) identical between the two replays |
| L8 late | c3 joins after c1/c2 are in s2/s3 | c3: `level walk seen` before its first owed pass, owed passes after it, signature equal to the server's, `mechanisms=K/K` (today's arena analogue `lp1_join` gives 52/45) |
| L11 cross | four clients s1->s2->s3, one stays in s1 on the P_Modul_25 bridge | as L8 cross; the s3 sack copy `P_Modul_26_*_R35` exact on every client at 0 ms |
| mismatch | one headless client run with a test switch that perturbs its signature | the server ends the session with "placement mismatch for ..."; the same as a late joiner |
| arena regression | the existing arena recipes (the 10 recent headless runs) | journals identical to the pre-change build except the header version and fields; `level walk seen at tick N` equal on every side |
| forced rollbacks | L8 cross with `--force-rollback` windows that contain the s2 pass | 0 bodies diverging vs the control (`worst_bodies.py`), `birth_recreate_failures` 0 |

**As built (step SP6 = S6, 2026-10-27).**

- **Where.** `BallanceMMOServer/sim/session_client.cpp` (the headless host),
  `session/client_core.hpp` (one accessor), `game/sector_placement.hpp` (the
  scene refusal); tools `.local-tools/arch_server_lp.sh` (ninth argument
  `placement_clones`), `.local-tools/wire_check.sh` (default set
  `core_ref_s6`, labels = the known arms present in the set). No protocol,
  journal, record or engine change; the server and the retail mod are
  unchanged in behaviour (the server compiles the shared header's refusal,
  which no anchor in the level scene reaches; the x86 mod compiles the
  unused accessor).
- **The core's part: `session_client_core::placement_active()`** =
  `capabilities().placement && world_options() & WORLD_PLACEMENT_CLONES`, the
  decision §2.4 gives the core (a host never re-derives it from the bits).
- **The headless host.** `make_capabilities` declares `placement` always
  (S5's switch no longer gates it). `anchor()` calls `place_sectors` after
  the Sequencer and proximity restores and before `capture_world_hash`,
  `session_script_selection`, `explosion_pieces` and the reset guard, as the
  §2.4 table and S2's call-site adaptation have it: with `placement_active()`
  it runs `apply` with the physics hooks the server's `place_sectors` uses
  (`list_bodies` names, `force_block_get`), logs `describe()` as
  `placement: ...` lines, and a refusal ends the session locally ("sector
  placement refused: ...") before SessionReady, the path of a failed hash.
  Option off: `holds_copies()` true releases first and refuses the anchor
  ("the level still holds placement copies (...)") if that did not clear it.
  Either way `placement signature %016llx (placement on|off, N copies)` is
  logged and the signature is `anchor_report::placement`.
  `--placement-signature HEX` stays as the "mismatch" test switch: it
  replaces the world's signature in the report and logs both.
- **Release before the engine goes** (S2's teardown finding): the registry is
  a member `placement_`, released in `boot_engine` before the engine reset
  (the restart's reboot), at `begin_session` (whatever the next session
  loads over), and in the client's destructor (before the members, the
  engine among them, are destroyed).
- **S2's open bullet (b), the scene: a refusal**, in `apply` itself rather
  than in each host (one implementation: the server, SimTool and the retail
  host of S10 get it too). Nothing has been made at that point, so the world
  is untouched. Every anchor logged so far is in the level scene (146 log
  lines of SP2's runs, both hosts).
- **`--sector-at SECTOR:TICK`** (repeatable): before the frame whose
  `next_tick()` reaches TICK, once the session is assigned, the host calls
  `core_.request_sector(SECTOR, "--sector-at")`, which stamps it
  `current_tick() + 1 + lead` and sends it as the own Sector input (the
  logged `sector N stamped for tick T (--sector-at, ...)`). **Repeatable
  `--beam`**: the first keeps its fields (`--leave-after-unphys` counts from
  it), later ones are kept in order and requested after `--beam` and
  `--beam2`, the first due one per frame (the core holds one beam request).
- **Checks (quick tier; about 11.5 minutes of wall clock with the
  re-capture, and 12 more at the review round for the `l8_cross` re-capture
  and the five option-off control runs that root-caused its divergence;
  tools and outputs in `.local-tools/work/SP6/`).** x64
  BUILD-OK, x86 CLIENT-BUILD-OK (no layout change, no objects deleted), Arch
  ARCH-BUILD-OK (`ballanced-3e4289e95f25+bmmo-bc88b8e392a5-dirty`), 530/530
  tests, GCC 29 ok.
  - **L8 cross at 0 ms** (one run, snapshots off, the server's
    `placement_clones` on; recipe in `maps/arena-test/core_ref_s6/README.md`):
    the server and all four clients log `placement: 39 copies, 1 shared (0
    rewritten)` and signature `91a138e4fd6e7254` (S2's V2 value), `level walk
    seen at tick 11`, `sector +2 at tick 309`, `+4 at 316` (held by the
    pacer), `+3 at 709`; `mechanisms=45/45` on every client; the copies'
    bodies `P_Modul_30_Wippe_R15` and `_R48` are in every client journal's
    LOCAL rows and the server's; the server journal replays matched
    (2241/2241 in the re-capture below). `first_divergence.py` (0.05 m):
    client 1 agrees with the server throughout; clients 0, 2, 3 diverge
    identically at tick 556 on the ORIGINAL s2 rope bridge
    `P_Modul_29_Platte02` (0.0987 m; every `Platte01..09` from 554, up to
    1.598 m) and on the original `P_Modul_01_Pusher` (from 554, first error
    1.377e-02 m, max 0.0952 m).
    - *Cause of the disagreement*: the queued G-c wake (world-agreement
      design §1.1). The server journal's BodyRevived records: client 1, whose
      ball rests on the copy `P_Modul_30_Wippe_R15` from 501, reports
      `P_Modul_29_Platte04`, `P_Modul_30_Wippe_R15` and `P_Modul_01_Pusher`
      at 554; client 2, whose ball reaches the same copy at 561, reports the
      same three at 614; clients 0 and 3 never. Each world makes the wake
      when its OWN ball arrives and the server at the first ball's (§1.1.3's
      signature).
    - *Controls* (review round; the option off, otherwise the same server and
      binaries; `maps/arena-test/sp6_ctl/`): the same recipe (`l8_cross_off`,
      client 1's ball falls where the see-saw would be) and two runs placing
      client 1's ball at the option-on pose at the check tick (`l8_pose_off`,
      `l8_pose2_off`) reproduce the Pusher half bit for bit (non-leaders
      diverge at 556, first error 1.377e-02 m at 554, max 0.0952 m, client 1
      exact): pre-existing, G-c. The bridge is woken in none of them, nor
      with the ball resting by the bridge's end (`l8_near_off`); a ball
      dropped onto a plate (`l8_bridge_off`) breaks it in every world alike
      (a contact, not a probe). So the bridge's cross-world cause is the same
      G-c wake, and its trigger needs the original `P_Modul_29` at its s2
      site, not the copy (root-caused after the chain, step RO1's docs;
      `.local-tools/work/SP6/bridge/`): the module's own 80 m XZ proximity
      probe (`TT Scaleable Proximity` #46782 -> `Physics WakeUp` #47032 ->
      `Platte04`) fires when a ball comes within 80 m of the ORIGINAL
      `P_Modul_29_MF` at s2. With the placement off, the union's s4 pass
      moves the pooled module's frames to s4 while its bodies sleep at s2,
      so `l8_near_off` / `l8_bridge_off` had the probe's anchor at s4 and
      never tested it; the placement-on worlds are the retail-faithful ones
      and the cross-world split is G-c. Not a placement fidelity bug (the
      roadmap §4 item 1 entry "SP6 bridge trigger" is closed as G-c).
  - **The arena regression** (one run, snapshots off, 150 ms, 4 clients,
    captured: `core_ref_s6/resync_snapoff`): signature `ecfbfd9b63a330b5`
    (placement off, 0 copies; S5's) and `level walk seen at tick 30` on the
    server and every client, server journal 3562/3562 matched; the captures
    replayed by rSP5 and rSP6: per client exactly one send differs (the
    SessionReady: `CAP_PLACEMENT` and the signature where rSP5 sends 0;
    besides the login, whose uuid is the clock's) and exactly one journal
    record (the header: build id and `anchor_placement`). Unchanged except
    the header, as §7.4 requires.
  - **The re-capture** `maps/arena-test/core_ref_s6/` (README with recipes
    and sha256s): `resync_snapoff`, `swing_snapoff`, `l8_cross` and the solo
    pair; `wire_check.sh - rSP6` ALL SAME (12/12 sends, 12/12 journals); solo1
    vs solo2 IDENTICAL, and solo1 against the protocol-11 `core_ref_rc1`
    solo: every TICK hash and LOCAL checkpoint identical, the header alone
    differs. At the review round `l8_cross` was re-captured with client 2's
    second `--beam` at 740 instead of 900 (at 900 its ball had died at 810,
    "no own body to beam", so no second beam was made): the second ball is
    made at 741 in s3 and the other clients see client 2's s2 body
    unphysicalized at 741; `LABELS=l8_cross wire_check.sh - rSP6` ALL SAME
    (4/4 sends, 4/4 journals).
- **Not exercised live**: the option-off release branch (no run has a level
  holding copies with the option off) and the scene refusal (every anchor is
  in the level scene). Both reach S10's retail host (exit to the menu and
  reload, §7.5 run 3).
- **`--sector-at` is a test switch and is not bounded** by the level's sector
  count (like `--start-sector`); a sector the level lacks is sent as an own
  Sector input as given.
- **The option-off policy and the hot-source hooks** are written out in this
  host and in the server's `physics_world::place_sectors` (CK and physics
  work, not a core decision); S10 lifts them into a shared helper in
  `game/sector_placement.hpp` instead of a third copy (roadmap §4 item 1).
- **Deferred** (roadmap §4 item 7's list): L8 late; the rest of
  `core_ref_rc1`'s and `core_ref_t3`'s arms re-captured at protocol 12; the
  two branches above that no run reaches.

**As built (step SP7 = S7, 2026-10-27; V9 passed at step RO1 except gap F
- see the results).**

- **Where.** SimTool only (`sim_tool.cpp`) and two unset hooks in
  `physics_world` (`world_options::before_level_load`, called right before
  the level request, and `before_anchor`, at the start of `anchor()`, before
  the restores and the placement; every world but SimTool's leaves them
  empty). Nothing under `BallanceMMOCommon`, the headless client, the
  server's behaviour or the retail mod changed; no protocol, journal, record
  or engine change.
- **`--perturb-ids N`** makes N plain `CKCID_OBJECT`s in `before_anchor` and
  destroys them in one call (ids are not reused: the copies land N above).
  **`--free-ids-before-load N`** makes N in `before_level_load` and destroys
  them with `CK_DESTROY_FREEID` **one call per object in ascending id
  order** (adaptation: a batch would push the free list in
  `FinishDestroyObjects`' dependency-map hash order, CKContext.cpp:1628-1641;
  the free list pops from the back, CKObjectManager.cpp:342-363). Both print
  the first copy's and its original's ids and how many copies are below
  their originals (`ids: copy P_Modul_30_MF_R15 id=51469, original
  P_Modul_30_MF id=59895 (below); 19 of 39 copies below their originals` on
  L8 with 20000; 20 of 53 on L11 - the level load takes the top of the free
  list, so not every copy is below).
- **`--dump-surfaces-at N`** now works in the drop scene too (before world
  tick N, the `--second-move` unit).
- **`--savestate-check` takes `--second-move`** (needs `--frames` and
  `--drop-second-sector`; not with `--legacy-restore` or trafo inputs): a
  move before T0 is made by the scene or the lead (the lead now runs through
  `scene_tick`), one inside [T0, T0+N) before that straight tick, one after
  the window is refused. With a pass in the window the binding is
  `bind_session_world` (the clients' recipe, as the late-sector check's):
  the pass recorded with `manager_activations::set_pass(t+1)` and the frame's
  pending activations with `record_pending(t+2)` (this mode's record of t
  is BEFORE step t, so the step at t is replayed as t+1), re-issued by
  `replay_activation`, every selected script written back
  (`replay_activation = true`), the window's births logged at t+1 from the
  event log (balls excluded) and destroyed / put back / made again by the
  replayed scripts, `tracked.mechanisms` re-read every tick. A chained
  round whose live world holds another body set than the record of its end
  (the movable names, or a body first born after T0 - fixed ones included -
  present or absent against its first birth tick) has that world re-made
  from T0: every body first born after T0 that has a body taken out newest
  first, its joint handles released, put back at its EARLIEST birth after
  T0; T0 restored; the straight ticks re-run with the moves; the result
  checked against the record of the round's end (body set and blob). PASS
  additionally needs a pass made, 0 recreate failures and 0 missing births.
  A failed premise is printed on the FAIL line (`[premise: ...]`) even when
  the round failed for another reason first.
- **Fix-up round (review of 2026-10-27).** The first build's re-make took
  out only the movable names absent at T0, so the pass's two FIXED bodies
  (`P_Modul_01_Filler`, `P_Modul_01_Rinne`) stayed, were seen as unknown
  bodies by the T0 restore and rechecked into collision detection at once:
  the re-made world failed in all 10 re-makes (hidden, the premise was
  printed only for an otherwise clean round). Fixed as above (10 re-makes,
  0 premise failures). Also: the replay's per-tick host hook now runs the
  server world's own pre-script work of a tick (`update_proximity_probes`,
  `update_ball_identity_reads`; `physics_world::tick` does it before the
  behaviour pass, the binding never calls `tick`): see V9.
- **`--savestate-inert-from-birth`** (a diagnostic, prints `DIAGNOSTIC`
  and exits 3, never a PASS): a round anchored before a FIXED window birth
  skips the restore identity and the replayed blobs before that birth's
  record and compares from it on - it measures whether the clients' recipe
  (rollback.hpp:1230-1248: the fixed birth kept inert, rechecked at its
  birth tick) is exact after the birth.
- **V7: PASS.** L8 (passes 2-5 at world ticks 40/60/80/100) and L11 (2-6 at
  40-120), 500 ticks, `--placement on`: `--perturb-ids 37`, `1000` and
  `--free-ids-before-load 20000` each give the 501 `tickhash` lines of
  `--perturb-ids 0` byte for byte (signatures `91a138e4fd6e7254` /
  `5d22d3b5a61c38cb` in every arm). Q5's across-worlds half: neutral.
- **V8: PASS.** L8, passes 2,3,4,5 vs 5,4,3,2, surfaces dumped before world
  tick 400: 132 bodies (47 copy bodies) in both, every body's surface
  identical (hash `08870ad9084b5e14` both). Q6: nothing to do.
- **Finding: an x64 teardown crash with copies below their originals -
  roadmap §4 item 6, root-caused.** Every L8 drop scene with `--placement on
  --free-ids-before-load 20000` that ran sector 1 crashes after its last
  tick, in the context reset (`headless_engine::~headless_engine` ->
  `CKContext::Reset` -> `CKObjectManager::OnCKReset` -> `DestroyObjects` ->
  `ExtraCallBack` -> `DestroyExtra` -> `ShowParticles`, reading
  `0x00007FF600000008`). It does not happen with the placement off and
  20000, with the placement on and 3000 (copies above), or with 0 ticks (no
  Extra initialized); L11 with 20000 does not crash. Cause (fix-up round,
  a diagnostic build listing the dynamic root scripts left after
  `release()`): 137 remain in both the crashing arm and `--perturb-ids 0`,
  among them the BUILDER's copies `P_Extra_Point_MF001..003` (not the
  placement's, which `release` orders), whose MF scripts have ids 51597 /
  52468 / 53339 with `--perturb-ids 0` and 56050 / 55179 / 54308 (falling)
  with 20000 freed ids. The context reset destroys them through
  `FinishDestroyObjects`' dependency-map iteration (CKContext.cpp:1628-1641,
  an id-hash order), so their particle blocks' DETACH can come before their
  Extra block's - the S2 mechanism (the x64 `SetLocalParameterObject`
  over-read, roadmap §4 item 6) on the builder's unordered copies, which the
  item already names. Confirmed: destroying those three MF scripts first in
  the world's destructor (S2's order, one call) makes the same run exit 0
  with an identical tick-hash stream (experiment only, not in the tree;
  `.local-tools/work/SP7/experiments/`). Retail x86 cannot meet it; V7's
  hashes are unaffected (the crash is after the last tick). The fix belongs
  to item 6 (order the builder's copies' teardown as `release` does, or the
  engine change it names).
- **V9: PASS except gap F after step RO1** (final numbers at the end of
  this item; below, the SP7 rounds as they were found: two causes fixed in
  the fix-up round, four remained) (L8 drop
  scene on the s1 see-saw, `--second-move 2` inside the window, 50 rounds,
  depth 48; `.local-tools/work/SP7/v9r.sh`, outputs `v9r/`).
  - window 31..230 (the default lead), move at 80: round 0 refused at the
    restore, "the PreSimulate queue differs" (live: `Physics WakeUp in
    Gameplay_Ingame`, four `PhysicsCollDetection in Sound_Manager`;
    recorded: none). The same command WITHOUT the move fails identically:
    the retail ball is parked at world tick 204 (`retail ball parked (body
    removed)`), its blocks retry from the PreSimulate queue, and the queue
    is never rewound - gap Q (rollback-engine-gaps-design §1.1),
    pre-existing.
  - window 210..409 (`--savestate-lead 180`), no move (control): **PASS,
    50 of 50** after the fix-up (47 of 50 failed before on `P_Modul_18_MF
    Script / TT Scaleable Proximity` "Current Distance": the block's
    ObjectA is the server world's rewired probe frame `BMMO_Prox_n`
    (physics_world.cpp:414-444), put on the nearest ball by
    `update_proximity_probes` in `physics_world::tick` before the scripts;
    the replay steps the scripts without `tick`, so a replayed proximity
    measured the frame where the live world's last tick left it - a
    harness gap of SimTool's server-world stand-in, fixed in the binding's
    per-tick hook; no client world rewires probes).
  - window 210..409, move at 260 (pass replayed as 261): 50 of 50 fail.
    10 live worlds re-made, 0 premise failures; 126 bodies destroyed at a
    restore and made again by the replay, 126 exactly at their birth
    record, 0 recreate failures, 0 missing.
    - 38 rounds: `P_Modul_18_Particle Script` flags `0x400000` vs 0 only
      (2 logical, 0 float) - gap F, queued.
    - 6 rounds anchored before the pass: restore identity "world - objects
      44 46": the pass's two FIXED bodies are not in the engine's plan (it
      tracks movable bodies, `tracked.mechanisms`), so the restore keeps
      them inert with a recheck at their birth tick (rollback.hpp:1230-1248)
      - by design, not the anchor's world. With
      `--savestate-inert-from-birth` the same 6 rounds replay 261..265 byte
      for byte and diverge at **266: "world - ov_nodes 59 58"**, the blob
      describe showing the physics_RT movable list in another order
      (straight `P_Modul_41004, 41001, 41006, 41003, 41005, 41002, 41`,
      replay `41006, 41005, 41004, 41003, 41002, 41001, 41`). The deferral
      is NOT the cause: destroying the fixed births at the restore instead
      (they are then made again by the replayed scripts; experiment only)
      makes the restore identity pass and diverges identically at 266; and
      the placement is NOT the cause: `--placement off` diverges identically
      at 266 (59 vs 58). So a rollback anchored before a sector pass is not
      exact 5 ticks after the replayed pass, independent of this step: some
      order state of the pass's bodies (the order the `P_Modul_41` pieces
      enter the movable list when they start moving) is not carried by the
      blob and not reproduced by the replayed activation. Root cause NOT
      established: the next measurement is the IVP / physics_RT order of
      the `P_Modul_41` pieces at their creation in the live frame vs the
      replayed one. It belongs to the sector-activation replay
      (world-agreement / rollback-engine track), not to the placement.
    - 6 rounds that end before the pass, on a re-made live world:
      `Gameplay_SectorManager / Activate Sector: parameter size` (1
      logical). Cause: `restore_parameter` (script_replay.hpp:1252) leaves
      a parameter whose size changed since the record untouched, and the
      sector manager's string locals (`Activate Sector` Pin 1 / Pin 2 /
      Groupname) and `Get Key Row`'s output Group Name change length with
      the sector a pass activates. Confirmed with an experiment build that
      writes a size-changed parameter with the base
      `CKParameter::SetValue` (resizes, no `DataChanged`): those 6 rounds
      then fail only on gap F. A shared record-restore change (headless,
      server-free, retail), not made here; in a client the replayed pass
      rewrites these values before they are read, so the effect is on the
      re-recorded script records of the ticks before the pass.
  - **Root-caused and fixed at step RO1** (2026-10-27,
    `.local-tools/work/SP7/r266/`; lockstep-without-snapshots.md "As built
    (step RO1)"): the tick-266 split is the replay's script ORDER - the live
    frame runs the active objects in `CKBehaviorManager::m_BeObjects` order
    (a hash-ordered append, then an unstable priority-only sort), the
    replay ran `script_replay::select`'s (-priority, CK id) order, and
    `P_Modul_41`'s seven pieces come from seven scripts, so they were
    created (and IVP revived them, and physics_RT listed them) in another
    order; values were equal. The "objects 44 46" restore identity is the
    deferral of the pass's two fixed bodies; the "parameter size" rounds are
    `restore_parameter` leaving a size-changed string. RO1: the host records
    its own live frame's script order and the replay runs in it; a
    window-born fixed body a selected script makes again is destroyed at
    the restore (with a guard) and made again by the replay; a size-changed
    string is written back without propagation.
  - **Final (step RO1's build `build-agent-rRO1`,
    `.local-tools/work/RO1/v9.sh`, outputs `.local-tools/work/RO1/v9/`):**
    window 210..409, move at 260: **50 of 50 rounds fail on gap F's
    window-end line only** (`P_Modul_18_Particle Script` flags `4194304` vs
    0; 44 "2 logical", 5 "2 logical" crossing a script reset, 1 "1
    logical" crossing a script reset; every FAIL at its round's last tick),
    0 premise failures, 10 live worlds re-made, 126 bodies destroyed at a
    restore and made again by the replay (126 exactly at their birth
    record), 0 recreate failures, 0 missing; 12 fixed window births
    destroyed at a restore and back after their birth tick, 0 missing;
    the live script order: 3 changes recorded, 1147 replayed frames ran in
    it. The six rounds anchored before the pass (0, 9, 15, 16, 48, 49) and
    the six "parameter size" rounds now fail on gap F only. No-move control:
    **PASS 50 of 50**. Not re-run (the change cannot reach them): V7, V8
    (straight runs, no rollback), the §7.4 forced rollbacks (the headless
    client's journals over `core_ref_s6` are identical or order-only
    different, see RO1's wire check), the `--free-ids-before-load 20000`
    arm (its teardown crash is roadmap §4 item 6; deferred).
  - the same with `--free-ids-before-load 20000`: the same kinds and counts,
    then the teardown crash above (exit 139).
- **§7.4 forced rollbacks: FAIL on a queued gap; the pass windows exact.**
  `core_ref_s6/l8_cross`'s four captures replayed by rSP7 with
  `--force-rollback 10:24` (`.local-tools/work/SP7/fr.sh`; the headless
  client is unchanged since rSP6, whose plain replays are identical to the
  live journals): every journal is identical to the control through the
  rewinds at 310-400 whose windows hold the s2 and s4 passes (309, 316),
  and first differs at the first rewind whose window holds a remote ball's
  death (c0's beam unphysicalizes its ball at 401: c1-c3 differ from 411;
  c0 from 511, after c1/c3's beams at 501). `worst_bodies.py` c1: the three
  other balls 0.36-0.38 m at 412, then the Pusher (G-c). The placement-off
  capture `sp6_ctl/l8_cross_off` with the same cadence shows the same three
  balls 0.36-0.38 m at 412 (and a checkpoint difference already at 322,
  where the option-on run is exact): gap L (a life ending inside the window
  is absent from the whole replay, rollback-engine-gaps-design §1.2),
  pre-existing. `birth_recreate_failures` is 5 on every client (3 in the
  live control): all are the own ball `Ball_Wood@212`, which destroy_body
  leaves on its parking path by design (session_host_world.hpp, the
  `own_body`/`ball` branch); module births 105 recreated, 105 exact, 0
  failed.

**As built (step HU1, 2026-10-27; host unification, the user's rule of
2026-10-27, `docs/roadmap.md` §2).** The placement's host logic of §2.4 is
one helper in `game/sector_placement.hpp`, written so the retail core host
can call it at S10 without a third copy:

- `place_at_anchor(placement, context, backend, active, fail_at_row, log,
  signature, error)`: with `active` the apply with `placement_hooks_for`'s
  hooks (all or nothing; the describe lines logged; a refusal is
  `error = "sector placement refused: ..."`), otherwise
  `prepare_unplaced`; then the signature and its line ("placement
  signature %016llx (placement on|off, N copies)", plus the unreadable
  table's reason). False: the host must not anchor.
- `prepare_unplaced(placement, context, log, error)`: the option-off policy
  ("Option off" above): `holds_copies` -> `release` -> refuse if it still
  holds.
- `placement_hooks_for(backend, bodies, fail_at_row)`: the physics half of
  step 3 over a host's backend (`physics_direct` by value, `physics_view` by
  pointer; `game/host_backend.hpp`): the anchor's body names (`list_bodies`)
  and the force blocks' controllers.

The roadmap named the shape `placement_hooks_for(manager)` /
`prepare_unplaced(context, why)`; it takes the backend instead of the
manager (the retail host has no `CKIpionManager`) and the placement object
and a log (the release and its lines are the policy's). The server's world
(`physics_world::place_sectors`), the headless host (`session_client.cpp`
`place_sectors`) and SimTool (`tool_placement_hooks`) call it; the log text
of each host is unchanged (the server prefixes `world: `). **Adaptation:**
the server's world had no option-off branch (the headless host had); it now
runs the same policy, as §2.4 says every host does. A fresh world's registry
is empty and no original PH row names an `_R<n>` object, so it changes
nothing: SimTool `--list-placement` on L1-L13 (the world unplaced) reached
every anchor without a "holds copies" line, and the server journals of
`core_ref_s6/l8_cross` and `resync_snapoff` replay matched with stdout
byte-identical to rSP8's (2993/2993 and 4153/4153; the reference README's
2241 and 3562 are the harness's replays taken while the server was still
recording - the stored `l8_cross/server.bmjr`, sha256 as recorded, replays
2993/2993 on rSP6, rRO1, rRO2 and rSP8 alike). **Shared limitation:**
`holds_copies` also counts a PH row whose object name ends in `_R<digits>`
(`has_row_suffix`, the stale-level check a retail level needs); a
third-party map with such an original name is refused at the anchor with
the option off - the headless host did so before HU1, the server's world
does too now. No stock level (L1-L13) or the arena has one; if a map ever
does, the suffix check belongs only on a host whose level can be stale
(retail, S10) and a fresh world uses the registry alone. The retail core host instantiates the three
templates with `const physics_view*` (compile-only); S10 calls
`place_at_anchor` at its anchor and adds the release points (one `release`
helper for the three hosts then: the server's destructor and the headless
host's `release_placement` differ only in when they log).

**As built (step SP8 = S8, 2026-10-27; measurement, quick tier).**

- **Tool change (SimTool only, S3's deferred item):** `--replay-session`
  now takes `--placement-trace PATH` (the §7.3 trace of the `--drop-sector`
  rows, default sector 1, after every replayed tick),
  `--placement-trace-player ID` (that player's ball line; implies
  `--placement-trace-ball`, which in a replay needs it) and
  `--drop-sectors-above N` (every Sector event of a sector above N skipped
  and counted). `check_scene_switches` refuses the two new switches without
  `--replay-session`, `--second-move` / `--drop-again` in a replay,
  `--list` with a trace, and (review fix-up) any other mode next to the
  trace or the drop switches in a replay (`--drop`, `--savestate-check`,
  `--late-run`, `--resync-check`, `--array-reads`, `--spawn-test`, ...:
  `run_replay_session` is dispatched first and reads none of them, so
  the other mode was ignored without a word). The trace line is written
  before a client journal's RESYNC record replaces the stepped world
  (server journals carry none, so both arms trace the same thing), and a
  dropped Sector event skips `ensure_player` (no player's first event is
  one in the two journals). Nothing else in `sim_tool.cpp` changed; nothing
  under `BallanceMMOCommon`, the headless client, the server or the retail
  mod; no protocol, journal or engine change. Scripts:
  `.local-tools/work/SP8/inv.sh` (the two replays in parallel and the trace
  comparison), `trace_cmp.py`, `body_residuals.py` (per-body max residual
  of each client's LOCAL rows against the server's), `cpu.sh`.
- **The networked invariant (§7.4 "L8 invariant (networked)"): PASS on L8
  and L11; the negative control fails.** Each server journal replayed twice
  by `build-agent-rSP8`, as recorded and with `--drop-sectors-above 1`,
  watched sector 1 with the ball of the client that stays there:
  - `core_ref_s6/l8_cross/server.bmjr` (placement on; c0 = p15089151 on the
    s1 see-saw): as recorded 2993/2993 ticks matched, 0 checkpoint
    mismatches; dropped arm 4 Sector events skipped, the replay leaves the
    recording at tick 313; the s1 traces (11 hierarchies + the ball, ticks
    8..3000, 35916 lines) **byte-identical**.
  - `sp8/l11_cross/server.bmjr` (the L11 cross below; c0 = p1630220977 on
    the s1 `P_Modul_25` bridge): as recorded 2573/2573 matched; dropped arm
    5 Sector events skipped, leaves the recording at 501 (the beams into
    the absent s2); the s1 traces (17 hierarchies + the ball, ticks
    27..2599, 46314 lines; at the end 15 movable bodies of which 4
    simulated, 7 joints, 4 force blocks) **byte-identical**.
  - Negative control `sp6_ctl/l8_cross_off/server.bmjr` (placement off,
    the same recipe): as recorded 3077/3077 matched; the traces **differ**
    from tick 313 (the s2/s4 passes at 309) on row 0 `P_Modul_30_MF` (the s1
    Wippe's frame moved), the s1 trafos, extras and `P_Modul_18`, from 713
    (the s3 pass at 709) on `P_Trafo_Paper_MF001` and `P_Modul_25_MF001`,
    and on the ball from 420: the networked trace sees the coupling V10
    saw in the drop scene, so the placement-on PASS is not a blind check.
- **L11 cross at 150 ms (§7.4; one live run, snapshots off, the server's
  `placement_clones` on; the Arch server of TR4c's tree
  `ballanced-3e4289e95f25+bmmo-2ff5c6b11cc8-dirty`, no server code changed
  since; `maps/arena-test/sp8/l11_cross/`, captures included).** Recipe:
  `BIN=build-agent-rSP8/BallanceMMOServer STORE_SET=sp8 SP6/s6_run.sh
  l11_cross snapoff 4 40 11 true -- --latency-ms 150 --sectors 1` with c0
  `--beam 115.885 -22.41 -75.984 400` (the s1 bridge row 14), c1
  `--sector-at 2:300 --beam 429.639 -42.793 -83.437 500` (the copy
  `P_Modul_37_MF_R30`), c2 `--sector-at 2:300 --sector-at 3:700 --beam
  431.639 -42.793 -83.437 560 --beam 644.713 -48.792 -196.227 760` (then
  the s3 sack copy row 35), c3 `--sector-at 2:300 --sector-at 3:700 --beam
  419.878 -42.793 -60.102 500 --beam 570.742 -51.488 -163.8 780` (the
  copies R21, then R56).
  - Server and all four clients: `placement: 53 copies, 0 shared`,
    signature `5d22d3b5a61c38cb` (V7's L11 value), `level walk seen at
    tick 30`, `sector +2 at tick 335`, `sector +3 at tick 735`;
    `mechanisms=54/54`, rebases 0, resyncs 0, far 0, starved 0, rb
    20/19/15/18; the harness's server-journal replay 1994/1994 matched
    (2573/2573 over the whole file above). 14 copy bodies in every client's
    LOCAL rows (`P_Ball_Stone_MF00x_R39..42`, `P_Modul_19_Flaps_R43`,
    `P_Modul_25_Bridge_R21/R56`, `P_Modul_25_Bridge001_R23`,
    `P_Modul_26_Rope001_R35`, `P_Modul_26_Sack001_R35`,
    `P_Modul_34_Kiste001_R55`, `P_Modul_34_Schiebestein001_R55`,
    `P_Modul_37_Bridge_R30/R48`).
  - Per-body residuals against the server (`body_residuals.py`, LOCAL vs
    full checkpoints, 54 bodies): the s3 sack copy `_R35` is exact on every
    client, but it was never touched (c2's ball left the R30 copy at 594,
    fell and was reported dead at 736, so its beam at 760 logged "no own
    body to beam"; the 0 ms criterion "the sack copy exact" with a ball on
    it is not shown by this run). Over 1 mm: `P_Modul_34_Schiebestein001`
    0.658 m / `P_Modul_34_Kiste001` 0.451 m (the ORIGINAL, from 520) on c0,
    c2, c3 and 0.3 mm on c1, whose ball is the one near it: the queued G-c
    leader-only wake's `P_Modul_34` signature; `P_Modul_01_Pusher` /
    `Pusher001` 0.0952 m from 454 on c1-c3, exact on c0, whose ball is on
    the s1 bridge: G-c's Pusher signature (L8's 0.0952 m); the s1 originals
    `P_Modul_25_Bridge001` 0.023 m and `P_Modul_37_Bridge` 0.018 m from
    478/492 on c1-c3 (c0's ball, a remote ball for them, on the s1 bridge),
    and the copies `P_Modul_37_Bridge_R30` 0.036 m on c0, c2, c3 (6.7 mm on
    c1, the ball's owner) and `P_Modul_25_Bridge001_R23` 0.022 m on c0, c2
    (the c2 ball rolling on from R30): a remote ball's contact with a
    mechanism at 150 ms, on copies and originals alike. Under 1 mm:
    `P_Modul_25_Bridge`, `_R21`, `_R56` 0.19 mm (the size and the bodies of
    RO2's queued class, a sleeping parented bridge's CK matrix re-derived at
    a restore; not re-derived here). No residual is specific to a copy:
    every copy residual has its original's counterpart or the G-c
    signature. Whether the residuals are "no worse than the same arm's
    control per body" needs the placement-off control run (deferred, see
    below).
  - c2's ball death at 736, the tick after its own `sector +3` at 735: it
    had been falling with no contact since 594; the death is a client-
    reported event every world applied alike (the server journal replays
    matched). Not a cross-world difference, but NOT root-caused, and it
    may be the placement's own: two hypotheses, neither checked in this
    tier. (i) Entering the next sector changes what kills a ball: the
    placement copies carry the originals' groups including `DepthTest`
    (`client2.log:60-63`, the s3/s6 `P_Ball_Stone_MF*_R39..R100`), so a
    ball falling through no active s2 depth volume could die only once
    s3's became active at 735 - which would break §1's "a player
    entering the next sector changes nothing in the previous one".
    (ii) The fall itself: from 567 c2's ball had a friction contact on
    the copy `P_Modul_37_Bridge_R30` with gap 0 and a remote ball
    (`Ball_Wood_BMMO_848630496`) pressing on it from 567; the R30 mindist
    lengths were negative throughout (-0.579 m at 561, -0.289 at 581,
    -0.220 at 583 after the 582 rollback to 561, -0.099 at 590; whether a
    negative length means penetration was not checked), the friction
    contact on R30 was gone at 581 save one at 589, and the ball had no
    contact and no mindist left at 594 (`client2.log:686-712`): the ball
    sank through the bridge copy's plank, which a remote ball's push or
    the copy's collision could explain. To verify: replay
    `sp8/l11_cross/server.bmjr` with the ball line of c2 (ticks
    560-736) and log which script/entity deletes the ball at 736 (the
    original's or a copy's depth volume, s2's or s3's, or the level's
    height check); then the same with the s3 Sector event dropped
    (`--drop-sectors-above 2`): if the ball then dies at another tick
    or not at all, (i) holds. Queued with deferred item (c) (roadmap
    §4 item 7, SP8 (g)).
- **§7.6 CPU, SimTool x64 half** (`cpu.sh 3000`, the drop scene with the
  parked second player passing into every sector 2..N at world ticks 40,
  60, ...; wall time of a 3000-tick run minus a 0-tick run, two
  repetitions, run alone; `.local-tools/work/SP8/cpu/times.txt`): per tick
  on / off in ms: L8 0.084 / 0.037 and 0.083 / 0.048; L9 0.079 / 0.050 and
  0.055 / 0.040; L10 0.051 / 0.067 and 0.107 / 0.008 (an off 0-tick
  outlier); L11 0.066 / 0.052 and 0.073 / 0.043; L12 (8 sectors) 0.040 /
  0.064 and 0.066 / 0.044. Every sector active with the bodies at rest
  costs at most 0.11 ms per tick, the on-off difference within the
  repetition scatter (about 0.03 ms): far inside the 15.15 ms tick. The
  0-tick runs show the one-off ANCHOR cost of the placement (plan, apply,
  log; not split): on minus off L8 +1.09 / +1.02 s, L9 +1.35 / +1.39, L10
  +1.42 / +1.21, L11 +1.48 / +1.45, L12 (62 copies) +2.22 / +2.11 s on
  x64; in the live L11 run the server logged the anchor at 09:48:06 and
  the placement at 09:48:07, and every client made SessionReady (no
  start-barrier miss). The retail x86 anchor time is §7.5 run 2's (S10).
- **Headless half (from the one live run, placement on only):** the
  clients' 5 s status at the end: t_rest 16-24, t_rep 246-331, t_max
  24-29, t_cap 1253-1306, resim 311-417 over about 1950 frames, rebases 0,
  resyncs 0. The on-vs-off comparison §7.6 asks for is deferred.
- **Checks (quick tier, about 11 minutes of command wall time):** x64
  BUILD-OK (`build-agent-rSP8`), 538/538 tests; no x86 or Arch build (no
  code they compile changed); the GCC syntax check not run: its file list
  does not include `sim_tool.cpp`, the only changed source; no wire check
  (no shared or headless code changed).
- **Deferred** (roadmap §4 item 7's list): §7.4 L8 cross at 150 ms (a
  second live run) and its networked invariant; the L11 cross's placement-
  off control at 150 ms (the per-body "no worse than control" judgment);
  the L11 cross at 0 ms with a ball on the s3 sack copy; §7.6 headless on vs
  off (L8 and an L9/L10/L12 cross); §7.6 SimTool x86 (`build/sim-x86-a3`
  predates S3's drop-scene switches); §7.6 retail x86 (S10).

### 7.5 Retail runs (core host)

1. Two retail clients and the local server on L8, crossing s1 -> s2 -> s3
   with the automation `sector` and `beam` verbs. Pass: the anchor log's
   `placement` line, signature, dependency words and every copy's census
   equal the server's; `level walk seen at tick N` with N equal to the
   server's; the `scripts` verb lists `P_Modul_30_MF Script@P_Modul_30_MF_R15`
   and it starts at the s2 pass; client journals vs server as in §7.4.
2. The same, then `session` end and a second session without a manual
   reload. Pass: the host logs the reload (`level ran a union: reloading`),
   the anchor follows a fresh load (`cores=0`), the plan makes the same copies
   as the first session, the signature and the anchor pose and surface hashes
   equal the server's, and SessionStart -> SessionReady stays inside the 8 s
   start barrier.
3. Exit to the menu and load L8 again. Pass: `release` logs the destroyed
   copies (or that the game had already destroyed them, which settles §9 Q2);
   the next session's `apply` makes them again with no clash.
4. A trafo copy: beam onto L8 row 19's paper trafo copy. Pass: the trafo
   happens (pieces, the ball type changes), settling §9 Q4.
5. Late join: a third retail client joins run 1's session while the others
   are in s2/s3. Pass: its log shows the manager state for the first 20
   frames from its anchor (a new trace line, `sector manager: tick t
   active|idle`), `level walk seen at tick N` with N equal to a fresh server
   anchor's, its owed passes after the walk, `mechanisms=K/K`, signature
   equal.
6. The legacy path (`session core off`) with the placement on: the session
   ends locally with "the placement needs the session core"; with the
   server's placement off, the legacy run 1 works as today.

### 7.6 CPU

- SimTool: L8, L9, L10, L11 and L12 with every sector active, 3,000 ticks,
  ms/tick with placement on and off (x64 and x86).
- Headless: "L8 cross" and an L9/L10/L12 cross with four clients spread over
  the level, at 150 ms, interval 2, on vs off (server config
  `physics_placement_clones=false` for the control): the 5 s `status`
  rollback cost lines (`t_rest`, `t_rep`, `t_cap`), resim ticks per second,
  rebases and resyncs.
- Retail x86: run 1 on L9 or L12 (the largest growth) at `net lag 150`,
  with the same cost lines from the retail status.
- Pass: no rebase or resync that the control does not have, and the p99
  frame within the 15.15 ms tick on the slowest harness machine and on the
  retail x86 client. If it fails, the options are §9 Q7.

**As built (step SP10 = S10, 2026-10-27; the retail core host, quick tier).**

- **Where.** Retail: `BallanceMMOClient/session/core_host.cpp` (the file-scope
  `placement_host` registry, `caps.placement`, the anchor call, the release
  points, the `session placement` verb, the session end's RO1/TR4c/S10 count
  line, the force hooks and the union pass on the shared helpers),
  `BallanceMMOClient.cpp` (one hook line in `OnPostExitLevel`),
  `BallanceMMOClient.h` (one non-virtual method, no data member),
  `automation/client_automation.cpp` (the verb routed),
  `session/physics_session_client.cpp` (the legacy local end). Shared:
  `game/sector_placement.hpp` (`release_placement`, and the copy of step 4,
  below), `entity/session.hpp` (a comment). Server: `physics_world.cpp` (the
  destructor calls `release_placement`), `config_manager.hpp` (the default,
  M3 below); the headless host's `release_placement` calls the helper. No
  protocol, journal, record or engine change.
- **The anchor** (§2.4's row): `core_host_anchor` calls the shared
  `place_at_anchor` with the bridge (`const physics_view*`), the core's
  `placement_active()` and the logger, after the Sequencer and proximity
  restores and before the hash, the script selection and
  `explosion_pieces` (as S2's call-site adaptation has it for every host); a
  refusal ends the session locally before SessionReady; the signature is
  `anchor_report::placement` with `caps.placement` declared, option on or
  off. The option-off release policy is the helper's (`prepare_unplaced`).
  No third copy of the policy.
- **The registry and the release.** A file-scope `placement_host` (the
  `BallanceMMOClient` class takes no member) outlives the session; the
  shared `release_placement` (one helper for the three hosts, HU1's queued
  item (c)) runs at `OnPostExitLevel` (after the session end or the
  reload's own exit) and at `OnLoadObject(isMap)` (leftovers of a missed
  exit), nothing when the registry is empty.
- **The legacy path** ends a session whose `world_options` has
  `WORLD_PLACEMENT_CLONES` locally at its SessionStart, before the restart
  ("the placement needs the session core (session core on)"); nothing else
  changes there.
- **Root cause found and fixed: the retail copy crashed (step 4).** Symptom
  (first run, `scripts/retail/sp10_run1`): the anchor ran on 8 consecutive
  frames, the first 7 leaving no line at all, the 8th refused "row 39: the
  source's P_Ball_Stone_MF001 has a physics body" (by then the restart's
  sector-1 pass had run), and the server ended the session ("nobody
  reported ready"). A diagnostic build (`sp10_diag1`; an SEH guard around
  the call, removed again) showed an access violation (c0000005) after row
  21's copy had completed, in row 23's. Cause: step 4 kept a
  `CKDependenciesContext` on this module's stack, as §2.3 says ("a context
  we keep"). In the Virtools SDK its members (`XHashID`, `XObjectArray`,
  `XBitArray`, ...) are inline templates allocating with `new[]`
  (VX_MALLOC off) and the class declares no destructor, so CK2.dll's
  `AddObjects` / `Copy` fill them from CK2.dll's heap and this module's
  inline destructor frees them with its own CRT (the same for
  `FillRemappedDependencies`' returned array); Ballanced is one module and
  never meets it. Fix (shared, one implementation): step 4 calls
  `CKContext::CopyObjects` with the hierarchy H (root first) as the sources,
  so the context lives and dies inside CK, and the returned array gives the
  copy of each member of H in that order (Ballanced CK2/CKContext.cpp:110-125:
  `GetObjects(i)` after `Copy`'s `m_Objects.Remap`); the root's preparation
  adds every child (CHILDREN), the listed children are already present
  (`CKERR_ALREADYPRESENT`), so the dependency map, the creation order and
  the copy are the root's, as before. That array replaces `dc.Remap` in steps
  5 and 8; the per-copy log line loses "(remapped N)" (S2's `made` count
  stays, and was equal to it on every copy). A first attempt with
  `CKContext::CopyObject` and a structural pairing of the two hierarchies
  failed: a copy's children are not in the source's order ("the copy of
  P_Modul_25_HingeFrame has another class", SimTool L11). Byte-identical on
  Ballanced: SimTool `--list-placement --placement on` on L8 and L11 (every
  census, fingerprint, signature, the release check PASS) equal to rHU1's
  but for "(remapped N)"; the wire check below, `l8_cross` with the
  placement on included.
- **Q1, the retail half (S1's deferred item).** The retail CK2 prints the
  same builder words as Ballanced (OBJECT 0x3, SCENEOBJECT 0x1, BEOBJECT
  0x7, 3DENTITY 0x2, MESH 0x2, the rest 0) and the same `m_Flags=0
  (CUSTOM) size=54`; it differs in one default word, BEOBJECT
  `default=0x3` (Ballanced 0x1), which the builder's custom words do not
  use, and in two class names of the "every nonzero builder word" line
  (`2D Entity` / `2dEntity`, `Grid Layer` / `Layer`). Logged, not hashed
  (§10 R1 #3). Q8 (the retail `Copy` against Ballanced's): every copy line
  of both anchors (census, groups, attributes) and the signature equal the
  server's; the `scripts` verb lists the copies' scripts (119 on L11, e.g.
  `P_Modul_25 script@P_Modul_25_MF_R21*`), the same set and active marks on
  both clients.
- **Q2, settled for the copies:** at `OnPostExitLevel` (the reload's exit)
  every recorded copy root and every recorded source was gone already
  ("release: P_Modul_25_MF_R21 (row 21) is gone already" ... "0 of 53 PH
  row(s) put back, 0 of 53 copies destroyed"): the game's level exit
  destroys the level with its dynamic copies before the hook runs. The
  release is a guard there; run 3's builder-copy leak check is not run.
- **M3: the server's `physics.placement_clones` defaults to true** from
  this step, as the design intends (§5); the retail run passed with the
  option on. A config that names it keeps its value (the harness configs
  do; so does every config.yml a server wrote since S5, whose `save()`
  writes the whole file back: a deployed server, okbc.st included, turns it
  on by editing its config). A legacy retail member (session core off) ends
  locally at SessionStart, before the restart, and first sends its usual
  SessionReady with no capabilities, so the server refuses it at once (no
  CAP_PLACEMENT; out of the session, not out of the room) instead of the
  other members waiting `kStartBarrierTimeout` (8 s) for the barrier to drop
  a member that went silent (SP10 review; the implement round had the local
  end without the send, which cost every session in a mixed room 8 s).
- **Review fix-ups.** (1) The legacy SessionReady above. (2) A level with
  copies counts as a level that ran a session (`level_ran_union`) as soon
  as `place_at_anchor` made copies, not only after the whole anchor: an
  anchor that made the copies and then failed (the world hash) used to
  leave them in a level the next countdown restarted without a reload,
  whose apply then refused ("this process already holds N placement
  copies") on every session until the player left the level. (3) Step 8's
  remap now covers the hierarchy H only (the `CopyObjects` array), as §2.3
  states it ("names a member of the hierarchy"); the old `dc.Remap` also
  followed any other object the copy duplicated, e.g. a mesh or material if
  the builder words (3DENTITY 0x2, MESH 0x2) copy one. Neither the listing
  nor the wire check sees an attribute's value (`entity_print` holds
  names), so an object-valued attribute naming such a non-member is not
  covered by the byte-identity above; the final verification's L8 run 1
  (§7.5) is where a difference would show as a body difference. Gate
  round (retail code only: the x86 build, CLIENT-BUILD-OK, and one retail
  run, `scripts/retail/sp10_gate1`, 55 s): L11, the placement on,
  snapshots off, client 1 `session core on`, client 2 `session core off`:
  the server sent SessionStart and refused client 2 in the same second
  ("refused: no CAP_PLACEMENT (caps 00000000)"), client 2 logged "ended:
  the placement needs the session core", client 1 was ready 5 s later
  (countdown + anchor, 53 copies, signature `5d22d3b5a61c38cb`), no "did
  not report ready", the session ran to tick 322 with a `sector 2`
  crossing; the session-end line "RO1: 6 kept ... TR4c: 160, 0 could not
  switch; placement (S10): on, 53 copies". The fix-up (2) path (a hash
  failure after the copies) was not provoked.
- **Checks (quick tier; about 13 minutes of wall clock with the
  root-cause rounds; tools in `.local-tools/work/SP10/`: `sp10_retail.py`,
  `sp10_judge.py`).** x64 BUILD-OK, x86 CLIENT-BUILD-OK (no layout change),
  Arch ARCH-BUILD-OK (`ballanced-3e4289e95f25+bmmo-48d1c3293644-dirty`),
  538/538 tests, GCC 29 ok. The headless BAR: `wire_check.sh rHU1 rSP10`
  on `core_ref_s6` ALL SAME (12 send traces SAME, 12 journals IDENTICAL),
  before and after the step-4 fix. **One two-copy retail run**
  (`scripts/retail/sp10_run2`; §7.5 runs 1 and 2 on L11 instead of L8: the
  Arch server with `placement_clones` on, `trafo_inputs` off, snapshots
  off; session 1 on the menu-loaded level, client 1 `sector 2` then
  `sector 3`; session 2 by the host's restart, client 1 `sector 2`): both
  sessions anchor on both clients with the server's hashes
  (`a8c7f832281a39c5 / a34811ce6a4225ed`) and placement signature
  `5d22d3b5a61c38cb` (S2's V2 value; all three journal headers carry it with
  `world_options=1`), 53 copies; session 2 reloads (map load 1.58-1.59 s,
  ready 2.20-2.21 s after the exit), SessionStart -> anchor 5.2 s (S9:
  3.75 s; the retail apply of 53 copies takes 1.43-1.45 s), inside the 8 s
  barrier; no mismatch. **Every placement body exact by name**: session 1,
  14 bodies (`P_Modul_25_Bridge_R21`, `_Bridge001_R23`,
  `P_Modul_37_Bridge_R30` from tick 126; `P_Ball_Stone_MF001..004_R39..42`,
  `P_Modul_19_Flaps_R43`, `P_Modul_26_Rope001_R35` / `Sack001_R35`,
  `P_Modul_34_Kiste001_R55`, ... from 328), client 1 vs client 2 over 300
  shared ticks and each against the server over 299: 0 body-set
  differences, 0 value differences (position, rotation, linear, angular);
  session 2, 3 bodies over 180 / 179 ticks, the same. The anchor's
  placement lines equal the server's but for the two Q1 lines above. RO1
  and TR4c run: "live script orders (RO1): 7 kept; journal checkpoint body
  reads at 53-bit precision (TR4c): 303, 0 could not switch" (session 2:
  6, 486-487, 0).
- **Deferred** (roadmap §4 item 7's list): §7.5 run 1 on L8 and runs 3-6;
  §7.6 retail x86 CPU; the headless A/B arms with the new default; CPU
  parity; three launches without a hang; a core-off retail session.

## 8. Steps

Each step: implement; review (an adversarial reviewer reads the diff against
this document and §6); gate (the checks listed; a failing gate is
investigated to its cause before the next step).

| step | implement | gate |
|---|---|---|
| S1 | `session/placement_plan.hpp` + `placement_plan_test.cpp`; SimTool `--list-placement` (block choice, dependency words, plan; read-only) | §7.1 plan tests; V1's block choice and plan on all levels; Q1 settled from the printed words |
| S2 | `game/sector_placement.hpp` (`apply` all-or-nothing, `signature`, `holds_copies`, `release`, log); `physics_world` option (default off) and call site; SimTool `--placement` (default off), `--placement-fail-at` | V1 complete, V2, V3, V4, V5, V11, V12; review of every name lookup listed in §4 |
| S3 | `--second-move`, `--placement-trace`, `scripts/placement_check.py` | §7.3 all arms PASS with placement on, V10 FAIL with it off |
| S4 | the union gate (SU) with its log line, its unit tests | V6; arena regression (§7.4) unchanged `sector +N` ticks in the 10 recent runs and equal `level walk seen` on every side; `lp1_join` client3 52/52. Protocol 9 and no copies anywhere (§5), so these networked runs need no S5. |
| S5 | protocol 12 (written 10 before A3 / T2 took 10 / 11): the `WORLD_PLACEMENT_CLONES` bit of `world_options`, `anchor_placement`, server plumbing and compare (§2.4), config default false until S10 (M3), journal v6, SimTool replay honouring the header, SimTool record default on, the plan bound 63 | §7.1 message and journal tests; a v5 arena journal still replays matched; §7.4 "mismatch" |
| S6 | headless host: anchor call, `anchor_report.placement`, option-off release path; `--sector-at`, repeatable `--beam` | §7.4 L8 cross (0 ms), L8 late, arena regression |
| S7 | `--savestate-check` with a pass inside the window; `--perturb-ids`, `--free-ids-before-load` | V7, V8, V9; §7.4 forced rollbacks |
| S8 | measurement only | §7.6 SimTool and headless; §7.4 L8/L11 cross at 150 ms; the networked invariant replay |
| S9 | retail level lifetime (§2.5), on today's placement-free retail path first: (1) measure the gap: two retail clients on L11, session 1 crossing s1->s3, end, session 2 without a reload; log the anchor hashes, `cores=` and the body list against the server's; (2) `level_ran_union` flag, the shared level-load function (from CA), the reload at the countdown, "Go!" after the load and the capture window | (1) recorded with its cause (expected: union sectors' bodies at the anchor, "world mismatch"; any other result is investigated to its cause); (2) the same sequence anchors with hashes equal to the server's and `cores=0`, inside the start barrier; three launches without a hang; a clean level still restarts without a load |
| S10 | retail core host: anchor call, `signature` in every anchor, option-off release path, `release` at level exit and map load, registry as file-scope static; legacy path ends locally on `WORLD_PLACEMENT_CLONES` | §7.5 runs 1-6; §7.6 retail x86 |
| S11 | docs: this file's as-built notes, lockstep doc "The sector union, as built", session-client-core design (host support list), protocol doc, `collision-overhaul-design.md` 9.x entry | review |

S4 is independent of S1-S3 and may land first. S5 must precede every
networked run that can build copies (S6, S8, S10); S9 precedes S10.

## 9. Open questions

The answers as built are §12.4. Open: Q4 (§7.5 run 4) and Q7's headless
and retail x86 halves (§7.6), both in the final verification; Q2's
builder-copy leak check (§7.5 run 3) did not run either.

| # | question | how to settle |
|---|---|---|
| Q1 | The builder's dependency flags: are attributes (BeObject bit 2) and groups (bit 4) copied, and which classes are copied? F1/F2 settle names, scripts, children and meshes; the flag word is only a pointer in the dumps. The design does not depend on the answer (steps 5 and 8-9 set names, attributes and groups explicitly and in a fixed order). | S1's `--list-placement` prints `GetClassDependencies` per class and the chosen block; the retail log of S10 prints the same and the censuses; §7.5 run 1 requires them equal. |
| Q2 | Does the game destroy dynamic copies at level exit? Event_handler `Exit Level / reset Level` deletes three groups (one is `Trafos`) with delete dependencies whose flags are pointers in the dump; `Delete Dynamic Objects` is only on the exit-to-system path (`audit/d_Event_handler.txt:68-69`). If the builder's copies leak, so would ours without `release`. | §7.5 run 3: after `OnPostExitLevel`, look up the recorded copies and a builder copy (`P_Modul_41_MF001`) by id and name before `release` runs, and log which exist. A leaking builder copy is a pre-existing retail finding: if run 3 shows one, its consequence (a later load's builder copies get other `UNIQUENAME` suffixes, F1, so retail body names differ from the server's) is checked in the same run by comparing the builder copies' names with the server's, and reported with options. |
| Q3 | After a session, vanilla play on the dirty level until the next load shows the earlier union sectors still physicalized (§2.5 "After the session"). No shared state is involved, but the player sees modules active in sectors vanilla would have torn down. | S9's measurement (1) shows what is left. The user decides between: (a) the session end reloads the level (the player restarts the level fresh); (b) leave it (the next session reloads anyway). |
| Q4 | Is a trafo copy picked up? Steps 8-9 rest on F6 and on trafos carrying attribute 53 in the file. | §7.5 run 4; headless: beam the own ball onto L8 row 19's pose and look for the explosion pieces in the client journal. |
| Q5 | Is the live behaviour order's dependence on CK ids (§3.2 item 3) neutral for physics across worlds, and does the replay's (priority, id) order equal the live order in a replayed frame in which copies and originals are born together? | V7 (across worlds: ids perturbed both ways) and V9 plus the §7.4 forced-rollback arm (within a world). If V7 fails: retail's CK2.dll cannot be changed, so the tie itself must go: (a) the CK step gives every copied BeObject a priority of its own derived from its row (`CKBeObject::SetPriority`, the original's priority minus a row-ordered offset smaller than the gap to the next priority in use), which makes the order of copies the plan's in every world; (b) none other without an engine change, which retail cannot take. If V9 fails: `execute_frame` follows the behaviour manager's own object order (`CKBehaviorManager::GetObject(pos)`) restricted to the selection instead of (priority, id). |
| Q6 | Do any copies of a shared Collision Surface name get different hulls depending on which row physicalizes first (§3.4)? | V8. If yes: (a) drop that surface's cached hull before each row's first Physicalize with the #9 entry point (a bridge call, no engine change), so each row compiles its own hull; this also changes today's shared hull of the builder's pooled boxes, so it needs the arena and L13 A/B before it lands; (b) if V8 shows equal hulls, nothing to do. |
| Q7 | CPU: does the larger selection fit the deployed interval 2 on L9, L10, L12 and on retail x86? | §7.6. If not: (a) leave out of the selection scripts proven to touch no physics (`P_Modul_18_Particle Script`: particles and a Rotate of its own frame, 183 parameters per copy); (b) skip capture and restore of a script never activated since the anchor (a "never started" bit in the record); (c) interval 3 for levels above a measured size. |
| Q8 | Does the retail CK2 `CKDependenciesContext::Copy` produce the same hierarchy, names (after the rename), meshes and scene flags as Ballanced's? | The signature and censuses at §7.5 run 1; on a mismatch, both hosts' logs print the per-row fingerprints, which name the row and the field. Scene flags are not in the signature: the `scripts` verb (retail) and `--list-scripts` (SimTool) must show the same active set after the anchor. |
| Q9 | Is re-placing the shared same-pose dome (L8 r41/r52, L3 r8/r16) a no-op for its body and contacts? **Answered at S3 for L8: yes** (arm L8-d: the dome's body and the ball on its crown at the pass are byte-identical with the placement on and off; as-built note S3 in §7.3; L3 r8/r16 not run). | §7.3 arm L8-d. If not: report; the choices are copying those rows too (against the decision, so the user decides) or skipping that row's pass with a pinned Set? (which brings back option A's re-assert trap). |

## 10. Review (revision 2)

Two reviews of revision 1 (correctness/determinism: 8 findings; feasibility
and rules: B1, S1-S5, N1-N4). Accepted and fixed:

| finding | fix |
|---|---|
| R1 #1, R2 B1: two Object Copy blocks | F4 corrected (37186 in `Copy Objects`, 37657 in `init and load Checkpoints`); §2.3 step 2 selects by graph (parent `Copy Objects`, Out to `TT Save IC`), refuses unless exactly one matches, logs both; V1 checks it. |
| R1 #2: share rows on a copy | `share` carries the holder's object; step 10 rewrites rows whose holder differs (L1 r38 -> `P_Trafo_Wood_MF_R37`); unit test, V1, V4 and §7.3 L1-a check it. |
| R1 #3: attribute type ids per process | the fingerprint hashes attribute type names (F6) and mesh names; the dependency words are logged, not hashed. (They are level data and equal in every world, so hashing them could not refuse a client by itself; they are dropped because they add nothing the fingerprint and the census do not.) |
| R1 #4: option off with copies present | every host computes and reports the signature at every anchor; option off with copies present releases them first or refuses (§2.4). §2.5 makes the case abnormal. |
| R1 #5: id layout | `--free-ids-before-load` arm in V7 and V9 puts copies below their originals; §2.3 and §3.2 restate creation order (fixed across rows, hash order inside a copy). |
| R1 #6: refused copy left in the world | `apply` is all or nothing (step 11), checked by V12. `Copy` itself already destroys its copies on its own failure (F5); the leftovers were ours, after it. |
| R1 #7: length rule | one rule, `> MAX_NAME`, checked for every renamed entity (step 5); tests at 64 and 65. |
| R1 #8, R2 S3: retail level-walk tick | the gate is an inactive-to-active edge (cannot open on a walk running at the anchor); every host logs `level walk seen at tick N`; §7.4 and §7.5 runs 1 and 5 require N equal; F10 records that all 197 retail anchors had the manager off. |
| R2 S1: legacy path | no placement on the legacy path; it ends locally on the option (§2.4); old run 5 replaced by §7.5 run 6. |
| R2 S2: run 2 waived Q3 | the gap is §2.5 now, built and measured in its own step S9 before the retail placement (S10); run 2 requires equal hashes and signature. The chosen fix is a reload, with the reason in §2.5. |
| R2 S4: CPU levels | §7.6 adds L9, L10, L12 and retail x86 with the rollback cost lines. |
| R2 S5: defaults and plumbing | `physics_world` defaults off until S5 (§5), S4's networked gate is exempt and says why; `world_ready_info` / `on_world_ready` / compare placement named (§2.4). |
| R2 N1: trafo copies at distinct poses | corrected (§3.2 item 2): copies coincide before their pass; the tie is resolved by list order, which steps 8-9 make equal in every world. |
| R2 N2: group append order | steps 8-9 remove and re-append in plan order, then HierarchyParser order (F7). |
| R2 N3: entities outside the hierarchy | step 5 refuses a copied 3D entity that is not a remap of a hierarchy member. |

Refuted (with the reason):

| finding | reason |
|---|---|
| R2 N4: Q10 defers a fix | Q10's premise was wrong, so there is nothing to fix or defer. P_Modul_01 has no proximity-driven Physicalize/Unphysicalize cycle: the script chooses Physicalize or Unphysicalize once per start from the activation-phase cell, and its proximity only wakes the Pusher (F14, `audit/d_P_Modul_01_MF_Script.txt:55-68`). Under the union no pass runs the Unphysicalize branch (N2), and with this design no later pass re-runs a copy's script, so the server/client difference the diagnosis (§6) suspected cannot arise. Q10 is removed; §2.7 states the P_Modul_01 behaviour. |

## 11. Decisions (user, 2026-10-23)

- **The rule, as stated by the user.** A player entering the next sector
  must not change any physics state of the previous sector; the activation's
  side effects on the previous sector are dropped, and where a dropped effect
  is what the next sector needs (its own instance), the next sector gets its
  own copy. Option B implements this.
- **Q3, after the session.** Leave the level as it is; the next session
  reloads it anyway (§2.5). No reload at session end.
- Chinese terminology: "sector" is 小节 (not 扇区).

## 12. As built: the track in one place (step S11, 2026-10-27)

The track is built: S1-S10 and this record. This section gathers what the
step notes above (As built S1 and S2 in §2.2/§2.3, S9 in §2.5, S4 in §2.6,
S5 in §5, S3 in §7.3, S6, S7, HU1, S8 in §7.4, S10 in §7.6) found and
measured, so a reader does not have to walk them; the notes stay the
detailed record, and where this summary and a note differ, the note is
right. Where the body of §1-§7 says something the code does not do, the
list in 12.2 says what the code does.

### 12.1 Steps and commits

| step | commit | what landed | its gate, as measured |
|---|---|---|---|
| S1 | cd501af | `session/placement_plan.hpp` (CK-free plan), the read-only half of §2.3 in `game/sector_placement.hpp`, SimTool `--list-placement` | V1's block choice and plan on all 14 levels exactly §1.4's table; Q1 settled (BEOBJECT 0x7: scripts, attributes and groups are copied); 505/505 tests |
| S2 | 5b536b2 | `sector_placement` (`apply` all or nothing, `signature`, `holds_copies`, `release`), the `physics_world` option (off), SimTool `--placement`, `--placement-fail-at` | V1 complete (every level's copies as §1.4's table: L8 39, L11 53, L12 62, the arena 0), V2-V5, V11 (x64 = x86 = Linux listings), V12 (refused at row 50: 7464 objects destroyed, 0 left); a teardown crash root-caused and fixed (`release` orders scripts root first) |
| S3 | 81c6803 | the invariant check I1: `--second-move`, `--placement-trace`, `scripts/placement_check.py` | I1 placement on: 8/8 arms PASS, traces byte-identical; V10 placement off: 8/8 FAIL, each after its first later pass |
| S4 | 72108fd (inside resync A5) | the union's level-walk gate (`gated_sector_union`), its log line and tests | V6 (L8: walk at 3, `+2` at 9, 65 objects = 43 + 22); arena `level walk seen at tick 30` on every side; `lp1_join` 52/52 |
| S9 | 5cd1c11 | retail level lifetime: a level that ran a session is reloaded at the next countdown | gap measured (61 bodies vs 48, `cores=2`, "world mismatch"); with the reload every anchor `a8c7f832281a39c5 / a34811ce6a4225ed cores=0`, SessionStart -> anchor 3.75 s |
| S5 | bc88b8e | protocol 12 / journal v6: `WORLD_PLACEMENT_CLONES`, `CAP_PLACEMENT`, `anchor_placement`, the server's compare, SimTool replay by the header, the plan bound 63 | 530/530 tests; a v5 journal replays 4112/4112; "mismatch" live (late joiner); a tampered v6 header refused |
| S6 | 0d1178b | the headless host (anchor call, signature, release, option-off policy), the scene refusal in `apply`, `--sector-at`, repeatable `--beam`; `core_ref_s6` re-captured | L8 cross at 0 ms: five sides `39 copies`, `91a138e4fd6e7254`, walk 11, `+2` 309 / `+4` 316 / `+3` 709, 45/45; non-leader residuals = G-c; arena regression: only SessionReady and the journal header differ |
| S7 | 3242755 (with RO1 97bddbb) | SimTool V7/V8/V9: id-layout hooks, a pass inside a `--savestate-check` window | V7 PASS (ids perturbed both ways, 501 tick hashes identical), V8 PASS (132 bodies, every surface equal); V9 PASS except gap F once RO1 replays in the live order (50/50 rounds fail only on F's window-end line, no-move control 50/50 PASS) |
| RO2 | 3ba04cb | the measurement of RO1's two uncovered cases (docs, a trace) | no value change over 300 settling ticks; the server and the four clients run one relative order (0 of 17 change ticks differ) |
| S8 | d0c7b28 | `--replay-session` with `--placement-trace`, `--placement-trace-player`, `--drop-sectors-above`; the measurement | networked invariant PASS on L8 (35916 lines) and L11 (46314 lines), the option-off control differs from 313; L11 cross at 150 ms: five sides `53 copies`, `5d22d3b5a61c38cb`, walk 30, `+2` 335 / `+3` 735, 54/54; SimTool x64 <= 0.11 ms per tick, anchor +1.0-2.2 s |
| HU1 | 48d1c32 | the placement's host logic (and three other host copies) as shared helpers, the user's rule "host-side logic is written once" (`docs/roadmap.md` §2) | byte-identical refactor: wire check ALL SAME, server journals 2993/2993 and 4153/4153 with identical stdout |
| S10 | fa8218d | the retail core host (anchor call, registry, release points), the copy through `CKContext::CopyObjects`, the legacy path's local end, `physics.placement_clones` on by default | one two-copy L11 run, two sessions with the reload: both clients the server's hashes and `5d22d3b5a61c38cb`, 53 copies, every placement body exact by name (14 bodies over 299-300 ticks, 0 differences); a mixed core-on / core-off room refuses the legacy member at once |
| S11 | the `docs:` commit after fa8218d | this section, the other documents' records | review |

### 12.2 What the code does (the design as built)

1. **The plan** (`session/placement_plan.hpp`, S1/S5). As §2.2, with
   `name` filled for every kind (keep: the source, copy: the new root,
   share: the holder's object, none: empty); every `plan_placements`
   call takes `game::kPlacementMaxName` = 63 (the bridge's body name
   holds 63 characters), so a 64-character copy root is refused by the
   plan; a copy name equal to another copy's or to any object a row names
   is an error; the signature's encoding is fixed (S1's note).
2. **The CK step** (`game/sector_placement.hpp`, S2/S6/S10). Runs after the
   Sequencer **and** the proximity restores, before the anchor hash, the
   script selection, `explosion_pieces` and the server's rewiring, in every
   world. Step 4 copies with `CKContext::CopyObjects` over the hierarchy H,
   root first (S10: a `CKDependenciesContext` kept in the mod's stack was
   filled by CK2.dll's heap and freed by the mod's CRT, an access violation
   on retail; Ballanced never meets it), and the returned array maps each
   member of H to its copy for steps 5 and 8. "What a copy made" is every
   object created during it. Destroys notify (no `CK_DESTROY_NONOTIFY`).
   Steps 8-9 act on the hierarchy's entities (scripts are not BeObjects).
   After step 10 the table is re-read and must re-plan to 0 copies and 0
   rewrites. `apply` refuses outside the level scene (S6). `release`
   destroys a copy's hierarchy scripts root first, then the rest (S2's
   teardown crash).
3. **The decision and the wire** (S5/S6). The core decides:
   `session_client_core::placement_active()` = the host's `placement`
   capability and the session's `WORLD_PLACEMENT_CLONES`. A host with the
   capability sends `CAP_PLACEMENT` and its signature (`anchor_placement`)
   option on or off; the server compares it right after the engine check,
   late joiners included, and refuses a member without the capability from
   a session with the option (out of the session, not the room). Protocol
   12, journal v6 (the header's trailing `anchor_placement`; client headers
   carry `world_options` too). SimTool `--replay-session` builds by the
   header and refuses a differing non-zero signature; `--replay` (record
   mode) applies the placement by default.
4. **Host logic, written once** (HU1/S10; the user's rule of 2026-10-27,
   `docs/roadmap.md` §2). `place_at_anchor` (the apply with its hooks, or
   the option-off policy, then the signature and its log line),
   `prepare_unplaced` (option off: `holds_copies` -> `release` -> refuse if
   it still holds), `placement_hooks_for(backend, bodies, fail_at_row)` and
   `release_placement`, all in `game/sector_placement.hpp`. Callers: the
   server's `physics_world::place_sectors` and destructor, the headless
   host's `place_sectors` / `release_placement`, SimTool's
   `tool_placement_hooks`, the retail core host's `core_host_anchor` and
   `core_host_release_placement`. Each host keeps only its backend, its log
   prefix and its release points. Adaptation at HU1: the server's world now
   runs the option-off policy too (a fresh world never holds copies).
5. **The release points.** Server: the world's destructor (before the
   engine). Headless: `boot_engine` before the engine reset, `begin_session`
   and the destructor. Retail: a file-scope `placement_host` registry (no
   `BallanceMMOClient` member), released at `OnPostExitLevel` and at
   `OnLoadObject(isMap)`; Q2 below makes the exit's release a guard.
6. **The level-walk gate** (§2.6, S4). `gated_sector_union` (derived, so
   `sector_union`'s layout is unchanged) in the server's world, both
   core hosts and the re-derivation; the timeout opens at tick 64; the
   resync bytes (protocol 10's) carry no gate. **The legacy retail path keeps
   the gate-free union** (no layout change, no feature) until R7.
7. **The retail level lifetime** (§2.5, S9). A file-scope `level_lifetime`;
   a set `ran_union` (also set as soon as `place_at_anchor` made copies,
   S10's fix-up) reloads the session's level at the start of the countdown
   by what the pause menu's "Exit Level" sends plus the menu driver's level
   start (Event_handler's `Load Level` does nothing from inside a level);
   "Go!" waits for the load and the Sequencer capture window; the reload
   gives up after 10 s (the server's start barrier is 8 s). No reload at
   session end (§11).
8. **The legacy retail path** ends a session with `WORLD_PLACEMENT_CLONES`
   locally at SessionStart, after sending its usual SessionReady with
   `caps` 0, so the server refuses it at once instead of the room waiting 8 s
   for the barrier (S10's review).
9. **The config.** `physics.placement_clones` defaulted to false from S5
   to S10 (adaptation M3: no retail host could build copies) and is true
   since S10. A config file that names the key keeps its value; every
   `config.yml` a server wrote since S5 names it (its `save()` writes the
   whole file), so a deployed server turns the placement on by editing it.

### 12.3 The numbers that identify a correct placement

| level | rows | copies | signature, placement on | signature, off |
|---|---|---|---|---|
| L8 | 86 | 39 (1 shared: r52 = r41) | `91a138e4fd6e7254` | `629f6128b4fc8389` |
| L11 | 105 | 53 | `5d22d3b5a61c38cb` | `4f444149dc156465` |
| arena | 25 | 0 | (nothing to copy: the table is the unplaced one; not run live with the option on) | `ecfbfd9b63a330b5` |

Every world computes the same values: SimTool x64 / x86 / Linux (V11),
the server and four headless clients (S6 L8, S8 L11), and the retail core
host on two game copies (S10 L11). The other levels' copy counts are §1.4's
table (V1). Cost: the plan and apply take +1.0 (L8) to +2.2 s (L12, 62
copies) at the x64 anchor and 1.43-1.45 s for L11's 53 copies on retail
x86, where SessionStart -> anchor became 5.2 s with the reload (S9's 3.75 s;
the start barrier is 8 s); with every sector active and the bodies at rest
a SimTool tick costs at most 0.11 ms (on and off within the scatter).

### 12.4 §9's questions, settled

| # | answer | where |
|---|---|---|
| Q1 | The builder's words: OBJECT 0x3, SCENEOBJECT 0x1, BEOBJECT 0x7, 3DENTITY 0x2, MESH 0x2, the rest 0, on all 14 levels in Ballanced and in the retail CK2 (which differs only in a default word the builder does not use, BEOBJECT `default=0x3`, and two class spellings; logged, not hashed). | S1, S10 |
| Q2 | The game's level exit destroys the placement's copies (and the sources) before `OnPostExitLevel` runs: "0 of 53 copies destroyed", all gone already; `release` is a guard there. The builder-copy leak check (§7.5 run 3) did not run. | S10 |
| Q3 | Decided (§11): no reload at session end; S9 reloads at the next session's countdown. | §11, S9 |
| Q4 | Open: §7.5 run 4 (a trafo copy, L8 row 19, `trafo_inputs` on) is deferred to the final verification. | roadmap §4 item 7, SP10 (a) |
| Q5 | Across worlds neutral (V7); within a world the replay ran in (priority, id) order, not the live order, which split L8's tick 266 - fixed at RO1 (the replay runs in the order the host's own live frame ran), measured at RO2 (a late activation changes no value over 300 ticks; hosts agree on the relative order). | S7, RO1, RO2 |
| Q6 | Nothing to do: V8, every surface equal whichever pass physicalizes first. | S7 |
| Q7 | SimTool x64 fits (<= 0.11 ms per tick); the headless on-vs-off cost lines and retail x86 are deferred. | S8; roadmap SP8 (d)/(e), SP10 (b) |
| Q8 | The retail `Copy` (through `CopyObjects` since S10) gives every copy line, census, group, attribute and the signature equal to the server's; the `scripts` verb lists the copies' scripts (119 on L11) alike on both clients. | S10 |
| Q9 | Yes for L8 (arm L8-d, byte-identical on and off); L3 r8/r16 not run. | S3 |

### 12.5 What is open (each queued)

- **The x64 teardown over-read** (Ballanced `SetLocalParameterObject`):
  the placement's copies are ordered by `release`, the builder's
  `*_MF001` copies are not; an x64 world torn down after an Extra block ran
  can crash, e.g. with copies below their originals (S7's
  `--free-ids-before-load 20000` arm). Roadmap §4 item 2.
- **c2's ball death at 736 in `sp8/l11_cross`**, the tick after its own
  `sector +3`: not root-caused; one hypothesis would break §1's rule (an s3
  depth volume, including a copy's `DepthTest` group membership). Roadmap
  §4 item 7, SP8 (g), with (c).
- **The residuals the crosses showed are not the placement's**: G-c
  leader-only wakes (P_Modul_01 Pusher 0.0952 m, P_Modul_34 0.45-0.66 m,
  item 5), remote-ball contact at 150 ms on copies and originals alike, gap
  L in the forced rollbacks (item 4), gap F in V9 (item 2).
- **Level 1**: its physics is frozen in the session world (the tutorial's
  time factor 0, S3) and a retail reload of L1 starts the tutorial (S9);
  both wait for K3 (the tutorial switched off, roadmap §4 item 6).
- **The `_R<digits>` check**: `holds_copies` also counts a PH row whose
  object name ends in `_R` and digits, so a third-party map with such an
  original name is refused with the option off (HU1's shared limitation);
  no stock level or the arena has one.
- **Step 8's remap covers H only** since S10: an object-valued attribute of
  a hierarchy member naming an object outside H is not remapped and not
  covered by any byte-identity check (expected none; roadmap SP10 (g)).
- **Never exercised live**: the option-off release branch on any host (no
  run had a level holding copies with the option off) and the scene
  refusal. Roadmap SP6 (c), HU1 (c), SP10 (a).
- **The checks the quick tier deferred**: roadmap §4 item 7, the entries
  SP5-SP10 and HU1 (among them §7.5 runs 1 on L8 and 3-6, §7.4 L8 late and
  L8 cross at 150 ms, the L11 cross's option-off control, §7.6 headless and
  retail x86, the headless A/B arms with the new default).
- **Host unification's queue** (HU1's inventory: the trafo set hooks, the
  retail level-file counters glue, the anchor sequence itself, N2 on every
  world, the 30-unmatched-snapshots rule): roadmap §4 item 1.

### 12.6 As built (step B2SW, 2026-10-27): the x64 teardown over-read, fixed

Engine change #21 (`docs/engine-changes.md`; CK2 `CKBehavior.cpp`, the
reimplementation, a submodule of Ballanced): `GetLocalParameterObject` and
`SetLocalParameterObject` move a `CK_ID` through a parameter wider than one
(a pointer parameter on x64) by its first 4 bytes, the rest zero, instead of
copying the parameter's whole size from or into the 4-byte id; a parameter of
4 bytes or less takes the old path (retail x86 cannot meet the case). Checks
(`.local-tools/work/B2SW/`): SP7's V9 on rR12: 50 FAIL lines (all gap F) and
the `--free-ids-before-load 20000` arm exit 139 at teardown; on rB2SW / rB2SWb
0 FAIL lines in all four arms, `free` exit 0. A teardown sweep L1-L13, the
placement on, the drop scene with sectors 1 / 2 and a move to 3 at 20, 120
ticks, `--tick-hashes`: rR12 and rB2SW both exit 0 with identical tick-hash
streams (121 lines each) on every level; the same sweep with
`--free-ids-before-load 20000`: rR12 crashes after its last tick on L6, L7, L8
and L9 (0xC0000005 reading 0x00007FF700000008, the S2 signature), rB2SW exits
0 on all 13 with tick hashes identical to rR12's. §12.5's first point is
closed.
