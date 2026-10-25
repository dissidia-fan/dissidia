//! Per-game manifest (`games/<id>/game.toml`) — TOML game configuration.
//!
//! Passed via `--config` (the manifest IS the config file, issue #46 P14).
//! Every key is optional; absence means "use the generic default". A
//! well-behaved game should recompile and boot with NO manifest at all —
//! the manifest exists for curation, never for bring-up table stakes.
//!
//! Schema (full sketch in `.planning/plans/46-47-runtime-portability-plan.md` §4):
//!
//!   [game]
//!   id    = "patapon"                 # compiled into recomp_game_config.h
//!   title = "Patapon (UCUS-98643)"    # informational only
//!
//!   [module]
//!   heap_base = "0x09000000"          # overrides the RECOMP_HEAP_BASE align policy
//!
//!   [boot]
//!   boot_path = "disc0:/PSP_GAME/SYSDIR/BOOT.BIN"   # default; EBOOT games override
//!
//!   [recompile]
//!   force_entries           = ["0x0887E6C0"]   # function starts Ghidra merged away
//!   force_entries_cross_mid = ["0x08827470"]   # only active under PSPRECOMP_CROSS_MID=1
//!   [[recompile.force_mid_entries]]            # mid-entries Ghidra missed
//!   entry  = "0x08827F44"
//!   parent = "0x08827E7C"
//!
//!   [runtime]
//!   asset_layer = "bnd"               # none|bnd ; default "none"
//!   host_heap_start = "0x09B80000"    # start of the host heap (default: heap_base policy)
//!   host_heap_end   = "0x0B800000"    # end of the host heap (default 0x0B000000)
//!   host_heap2_base = "0x0B810000"    # optional second host-heap region (with host_heap2_end)
//!   host_heap2_end  = "0x0BF00000"
//!
//!   # Existing arrays keep working at top level:
//!   [[stubs]]   name = "sceUtilityGetSystemParamInt"
//!   [[skips]]   address = "0x08804000"  reason = "..."
//!   [[patches]] address = "0x08810004"  instruction = "0x00000000"  reason = "..."
//!
//! `[game]`/`[boot]`/`[module]`/`[runtime]` flow into the generated
//! `output/include/recomp_game_config.h`; the runtime never parses TOML.

use serde::Deserialize;

/// Default end of the host heap (the historical hard-coded cap).
pub const DEFAULT_HOST_HEAP_END: u32 = 0x0B00_0000;

/// Top-level game configuration loaded from a TOML manifest.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct GameConfig {
    /// `[game]` identity table (id compiled into recomp_game_config.h).
    #[serde(default)]
    pub game: Option<GameSection>,
    /// `[module]` overrides of analysis.json-derived facts.
    #[serde(default)]
    pub module: Option<ModuleSection>,
    /// `[boot]` boot-path override.
    #[serde(default)]
    pub boot: Option<BootSection>,
    /// `[runtime]` runtime-layer choices.
    #[serde(default)]
    pub runtime: Option<RuntimeSection>,
    /// `[recompile]` curated emission inputs (force entries).
    #[serde(default)]
    pub recompile: Option<RecompileSection>,
    /// Functions to replace with no-op HLE stubs (by name).
    #[serde(default)]
    pub stubs: Vec<StubEntry>,
    /// Functions to skip entirely (emitted as empty stubs).
    #[serde(default)]
    pub skips: Vec<SkipEntry>,
    /// Instruction-level patches (override a specific address with a new word).
    #[serde(default)]
    pub patches: Vec<PatchEntry>,
    /// Overrides for batch size (default: 50 or CLI --batch-size).
    #[serde(default)]
    pub functions_per_file: Option<usize>,
}

/// `[game]` — manifest identity.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct GameSection {
    /// Game id; must match the `-DPSPRECOMP_GAME=<id>` runtime build.
    #[serde(default)]
    pub id: String,
    /// Informational title; never consumed programmatically.
    #[serde(default)]
    #[allow(dead_code)]
    pub title: String,
}

/// `[module]` — overrides of analysis.json-derived module facts.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct ModuleSection {
    /// Hex address pinning the runtime heap base (overrides the
    /// align-to-16MB RECOMP_HEAP_BASE policy, plan decision P11/O1).
    #[serde(default)]
    pub heap_base: Option<String>,
}

/// `[boot]` — boot-path override.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct BootSection {
    /// Guest exec path passed to module_start (EBOOT games override).
    #[serde(default)]
    pub boot_path: Option<String>,
}

/// `[runtime]` — runtime-layer choices.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct RuntimeSection {
    /// Asset layer selection: "none" (default) or "bnd".
    #[serde(default)]
    pub asset_layer: Option<String>,
    /// Start of the host heap (default: the heap_base policy / `[module] heap_base`
    /// pin). Hex string.
    #[serde(default)]
    pub host_heap_start: Option<String>,
    /// End (exclusive) of the host heap that serves sceKernelAllocMemoryBlock /
    /// partition allocs; default 0x0B000000. Hex string.
    #[serde(default)]
    pub host_heap_end: Option<String>,
    /// Optional second host-heap region [host_heap2_base, host_heap2_end),
    /// used when the first is full (e.g. above a module loaded inside the
    /// first region's range). Both keys or neither.
    #[serde(default)]
    pub host_heap2_base: Option<String>,
    #[serde(default)]
    pub host_heap2_end: Option<String>,
}

/// `[recompile]` — curated per-game emission inputs.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct RecompileSection {
    /// Function starts Ghidra merged into larger functions (hex strings).
    /// Replaces the former hardcoded FORCE_ENTRIES const (issue #46).
    #[serde(default)]
    pub force_entries: Vec<String>,
    /// Address ranges ("0xSTART-0xEND", end exclusive) that this set must
    /// NOT emit functions for: a window inside the image that other code
    /// (runtime-loaded overlays) occupies at run time. Functions Ghidra or
    /// discovery minted there would shadow the overlay resolver.
    #[serde(default)]
    pub exclude_ranges: Vec<String>,
    /// Force entries only active under PSPRECOMP_CROSS_MID=1 (former
    /// FORCE_ENTRIES_D2 const — see the D2/19G/19H notes in game.toml).
    #[serde(default)]
    pub force_entries_cross_mid: Vec<String>,
    /// Mid-entries Ghidra missed: {entry, parent} hex-address pairs
    /// (former FORCE_MID_ENTRIES const).
    #[serde(default)]
    pub force_mid_entries: Vec<ForceMidEntry>,
}

/// One forced mid-entry: an entry address inside an existing parent function.
#[derive(Debug, Clone, Deserialize)]
pub struct ForceMidEntry {
    /// Mid-entry address (hex string).
    pub entry: String,
    /// Parent function start address (hex string).
    pub parent: String,
}

/// A function name to replace with an HLE no-op stub.
#[derive(Debug, Clone, Deserialize)]
pub struct StubEntry {
    /// Function name as it appears in analysis.json.
    pub name: String,
}

/// A function address to skip entirely (emitted as an empty stub).
#[derive(Debug, Clone, Deserialize)]
pub struct SkipEntry {
    /// Hex address string, e.g. "0x08804000".
    pub address: String,
    #[serde(default)]
    #[allow(dead_code)]
    pub reason: String,
}

/// An instruction-level patch replacing a word at a specific address.
#[derive(Debug, Clone, Deserialize)]
pub struct PatchEntry {
    /// Hex address string.
    pub address: String,
    /// Replacement instruction word as hex string.
    pub instruction: String,
    #[serde(default)]
    #[allow(dead_code)]
    pub reason: String,
}

impl GameConfig {
    /// Game id ("" with no manifest / no `[game]` table).
    pub fn game_id(&self) -> &str {
        self.game.as_ref().map(|g| g.id.as_str()).unwrap_or("")
    }

    /// Boot exec path (PSP-universal default when unset).
    pub fn boot_path(&self) -> &str {
        self.boot
            .as_ref()
            .and_then(|b| b.boot_path.as_deref())
            .unwrap_or("disc0:/PSP_GAME/SYSDIR/BOOT.BIN")
    }

    /// Manifest heap-base pin, parsed (0 = no override; use the align policy).
    pub fn heap_override(&self) -> anyhow::Result<u32> {
        match self.module.as_ref().and_then(|m| m.heap_base.as_deref()) {
            None => Ok(0),
            Some(s) => parse_hex(s)
                .ok_or_else(|| anyhow::anyhow!("[module] heap_base is not a hex address: {s:?}")),
        }
    }

    /// True when `[runtime] asset_layer = "bnd"`.
    pub fn asset_layer_is_bnd(&self) -> anyhow::Result<bool> {
        match self.runtime.as_ref().and_then(|r| r.asset_layer.as_deref()) {
            None | Some("none") => Ok(false),
            Some("bnd") => Ok(true),
            Some(other) => anyhow::bail!(
                "[runtime] asset_layer must be \"none\" or \"bnd\", got {other:?}"
            ),
        }
    }

    /// `[runtime] host_heap_start`, parsed (0 = use the heap_base policy).
    pub fn host_heap_start(&self) -> anyhow::Result<u32> {
        match self.runtime.as_ref().and_then(|r| r.host_heap_start.as_deref()) {
            None => Ok(0),
            Some(s) => {
                let v = parse_hex(s).ok_or_else(|| {
                    anyhow::anyhow!("[runtime] host_heap_start is not a hex address: {s:?}")
                })?;
                if v < 0x0880_0000 || v >= self.host_heap_end()? {
                    anyhow::bail!("[runtime] host_heap_start {v:#X} must lie in 0x08800000..host_heap_end");
                }
                Ok(v)
            }
        }
    }

    /// `[runtime] host_heap_end`, parsed (default 0x0B000000).
    pub fn host_heap_end(&self) -> anyhow::Result<u32> {
        match self.runtime.as_ref().and_then(|r| r.host_heap_end.as_deref()) {
            None => Ok(DEFAULT_HOST_HEAP_END),
            Some(s) => {
                let v = parse_hex(s).ok_or_else(|| {
                    anyhow::anyhow!("[runtime] host_heap_end is not a hex address: {s:?}")
                })?;
                if !(0x0800_0000..=0x0C00_0000).contains(&v) {
                    anyhow::bail!("[runtime] host_heap_end {v:#X} outside 0x08000000..=0x0C000000");
                }
                Ok(v)
            }
        }
    }

    /// `[runtime] host_heap2_base/host_heap2_end`, parsed; (0, 0) = no second region.
    pub fn host_heap2(&self) -> anyhow::Result<(u32, u32)> {
        let r = self.runtime.as_ref();
        let b = r.and_then(|r| r.host_heap2_base.as_deref());
        let e = r.and_then(|r| r.host_heap2_end.as_deref());
        match (b, e) {
            (None, None) => Ok((0, 0)),
            (Some(b), Some(e)) => {
                let pb = parse_hex(b).ok_or_else(|| {
                    anyhow::anyhow!("[runtime] host_heap2_base is not a hex address: {b:?}")
                })?;
                let pe = parse_hex(e).ok_or_else(|| {
                    anyhow::anyhow!("[runtime] host_heap2_end is not a hex address: {e:?}")
                })?;
                if pb >= pe || pb < self.host_heap_end()? || pe > 0x0C00_0000 {
                    anyhow::bail!(
                        "[runtime] host_heap2 {pb:#X}..{pe:#X} must be non-empty, above host_heap_end and within 0x0C000000"
                    );
                }
                Ok((pb, pe))
            }
            _ => anyhow::bail!("[runtime] host_heap2_base and host_heap2_end must be set together"),
        }
    }

    /// `[recompile] force_entries`, parsed.
    pub fn force_entries(&self) -> anyhow::Result<Vec<u32>> {
        parse_hex_list(
            self.recompile.as_ref().map(|r| r.force_entries.as_slice()).unwrap_or(&[]),
            "force_entries",
        )
    }

    /// `[recompile] exclude_ranges`, parsed as (start, end) pairs.
    pub fn exclude_ranges(&self) -> anyhow::Result<Vec<(u32, u32)>> {
        let items = self.recompile.as_ref().map(|r| r.exclude_ranges.as_slice()).unwrap_or(&[]);
        items.iter().map(|s| {
            let (a, b) = s.split_once('-').ok_or_else(|| anyhow::anyhow!("exclude_ranges entry {s:?} is not START-END"))?;
            let lo = parse_hex(a.trim()).ok_or_else(|| anyhow::anyhow!("exclude_ranges start not hex: {a:?}"))?;
            let hi = parse_hex(b.trim()).ok_or_else(|| anyhow::anyhow!("exclude_ranges end not hex: {b:?}"))?;
            Ok((lo, hi))
        }).collect()
    }

    /// `[recompile] force_entries_cross_mid`, parsed.
    pub fn force_entries_cross_mid(&self) -> anyhow::Result<Vec<u32>> {
        parse_hex_list(
            self.recompile
                .as_ref()
                .map(|r| r.force_entries_cross_mid.as_slice())
                .unwrap_or(&[]),
            "force_entries_cross_mid",
        )
    }

    /// `[recompile] force_mid_entries` as (entry, parent) pairs, parsed.
    pub fn force_mid_entries(&self) -> anyhow::Result<Vec<(u32, u32)>> {
        let entries = self
            .recompile
            .as_ref()
            .map(|r| r.force_mid_entries.as_slice())
            .unwrap_or(&[]);
        entries
            .iter()
            .map(|fme| {
                let entry = parse_hex(&fme.entry).ok_or_else(|| {
                    anyhow::anyhow!("force_mid_entries entry is not hex: {:?}", fme.entry)
                })?;
                let parent = parse_hex(&fme.parent).ok_or_else(|| {
                    anyhow::anyhow!("force_mid_entries parent is not hex: {:?}", fme.parent)
                })?;
                Ok((entry, parent))
            })
            .collect()
    }
}

/// Parse "0x..."/plain-hex address strings (manifest convention: hex strings).
fn parse_hex(s: &str) -> Option<u32> {
    let t = s.trim().trim_start_matches("0x").trim_start_matches("0X");
    u32::from_str_radix(t, 16).ok()
}

fn parse_hex_list(items: &[String], what: &str) -> anyhow::Result<Vec<u32>> {
    items
        .iter()
        .map(|s| {
            parse_hex(s)
                .ok_or_else(|| anyhow::anyhow!("[recompile] {what} entry is not hex: {s:?}"))
        })
        .collect()
}

/// Load game config from a TOML file path. Returns default config if path is None.
pub fn load_config(path: Option<&std::path::Path>) -> anyhow::Result<GameConfig> {
    match path {
        None => Ok(GameConfig::default()),
        Some(p) => {
            let text = std::fs::read_to_string(p)
                .map_err(|e| anyhow::anyhow!("Cannot read config {}: {e}", p.display()))?;
            let cfg: GameConfig = toml::from_str(&text)
                .map_err(|e| anyhow::anyhow!("Invalid TOML in {}: {e}", p.display()))?;
            Ok(cfg)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_toml_parses_to_defaults() {
        let cfg: GameConfig = toml::from_str("").unwrap();
        assert!(cfg.stubs.is_empty());
        assert!(cfg.skips.is_empty());
        assert!(cfg.patches.is_empty());
        assert!(cfg.functions_per_file.is_none());
        assert_eq!(cfg.game_id(), "");
        assert_eq!(cfg.boot_path(), "disc0:/PSP_GAME/SYSDIR/BOOT.BIN");
        assert_eq!(cfg.heap_override().unwrap(), 0);
        assert!(!cfg.asset_layer_is_bnd().unwrap());
        assert_eq!(cfg.host_heap_end().unwrap(), 0x0B00_0000);
        assert_eq!(cfg.host_heap_start().unwrap(), 0);
        assert_eq!(cfg.host_heap2().unwrap(), (0, 0));
        assert!(cfg.force_entries().unwrap().is_empty());
        assert!(cfg.force_entries_cross_mid().unwrap().is_empty());
        assert!(cfg.force_mid_entries().unwrap().is_empty());
    }

    #[test]
    fn stub_entry_parsed() {
        let toml = r#"
        [[stubs]]
        name = "sceKernelSleep"
        "#;
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.stubs.len(), 1);
        assert_eq!(cfg.stubs[0].name, "sceKernelSleep");
    }

    #[test]
    fn functions_per_file_override() {
        let toml = "functions_per_file = 100\n";
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.functions_per_file, Some(100));
    }

    #[test]
    fn manifest_tables_parsed() {
        let toml = r#"
        [game]
        id = "patapon"
        title = "Patapon (UCUS-98643)"

        [module]
        heap_base = "0x09000000"

        [boot]
        boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"

        [runtime]
        asset_layer = "bnd"

        [recompile]
        force_entries = ["0x0887E6C0"]
        force_entries_cross_mid = ["0x08827470", "0x0882744C"]

        [[recompile.force_mid_entries]]
        entry = "0x08827F44"
        parent = "0x08827E7C"
        "#;
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.game_id(), "patapon");
        assert_eq!(cfg.boot_path(), "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");
        assert_eq!(cfg.heap_override().unwrap(), 0x0900_0000);
        assert!(cfg.asset_layer_is_bnd().unwrap());
        assert_eq!(cfg.force_entries().unwrap(), vec![0x0887_E6C0]);
        assert_eq!(
            cfg.force_entries_cross_mid().unwrap(),
            vec![0x0882_7470, 0x0882_744C]
        );
        assert_eq!(
            cfg.force_mid_entries().unwrap(),
            vec![(0x0882_7F44, 0x0882_7E7C)]
        );
    }

    #[test]
    fn host_heap_keys_parsed_and_validated() {
        let toml = "[runtime]\nhost_heap_start = \"0x09B80000\"\nhost_heap_end = \"0x0B800000\"\nhost_heap2_base = \"0x0B810000\"\nhost_heap2_end = \"0x0BF00000\"\n";
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.host_heap_start().unwrap(), 0x09B8_0000);
        assert_eq!(cfg.host_heap_end().unwrap(), 0x0B80_0000);
        assert_eq!(cfg.host_heap2().unwrap(), (0x0B81_0000, 0x0BF0_0000));
        // one half only, overlapping the first region, or past user RAM: rejected
        for bad in [
            "[runtime]\nhost_heap2_base = \"0x0B810000\"\n",
            "[runtime]\nhost_heap_end = \"0x0B800000\"\nhost_heap2_base = \"0x0B000000\"\nhost_heap2_end = \"0x0BF00000\"\n",
            "[runtime]\nhost_heap2_base = \"0x0B810000\"\nhost_heap2_end = \"0x0C100000\"\n",
        ] {
            let cfg: GameConfig = toml::from_str(bad).unwrap();
            assert!(cfg.host_heap2().is_err(), "{bad}");
        }
        // a start at or above the (default) end is rejected
        let cfg: GameConfig = toml::from_str("[runtime]\nhost_heap_start = \"0x0B100000\"\n").unwrap();
        assert!(cfg.host_heap_start().is_err());
    }

    #[test]
    fn bad_asset_layer_rejected() {
        let toml = "[runtime]\nasset_layer = \"zip\"\n";
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert!(cfg.asset_layer_is_bnd().is_err());
    }

    #[test]
    fn bad_force_entry_rejected() {
        let toml = "[recompile]\nforce_entries = [\"not-hex\"]\n";
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert!(cfg.force_entries().is_err());
    }
}
