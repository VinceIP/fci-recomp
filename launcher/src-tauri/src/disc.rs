//! Says at once what a chosen disc is: an extracted folder or a disc image (an XISO, or a full dump with
//! the game partition further in), as tools/disc.py reads them. Only default.xbe is read and hashed here,
//! which takes a moment; the build checks every file against disc.json before it copies anything.

use serde::Serialize;
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::titles::{Title, TITLES};

const SECTOR: u64 = 2048;
const MAGIC: &[u8] = b"MICROSOFT*XBOX*MEDIA";
/// where the game partition starts: an XISO, then the full dumps of the three disc layouts
const PARTITIONS: [u64; 4] = [0, 0x1830_0000, 0x0FD9_0000, 0x0208_0000];

#[derive(Serialize, Clone, Default)]
pub struct DiscCheck {
    pub path: String,
    pub ok: bool,
    /// the title's key when the disc is that game, supported or not
    pub title: Option<String>,
    pub headline: String,
    pub message: String,
    /// title ID, version, region as the disc says
    pub detail: String,
}

/// the files in the root folder of a disc: name -> (offset in the image file, size)
fn root_files(f: &mut File) -> Result<Vec<(String, u64, u64)>, String> {
    let mut head = vec![0u8; SECTOR as usize];
    for base in PARTITIONS {
        if f.seek(SeekFrom::Start(base + 32 * SECTOR)).is_err() || f.read_exact(&mut head).is_err() {
            continue;
        }
        if &head[..20] != MAGIC || &head[0x7EC..0x7EC + 20] != MAGIC {
            continue;
        }
        let sector = u32::from_le_bytes(head[0x14..0x18].try_into().unwrap()) as u64;
        let size = u32::from_le_bytes(head[0x18..0x1C].try_into().unwrap()) as u64;
        let mut table = vec![0u8; size.min(16 << 20) as usize];
        f.seek(SeekFrom::Start(base + sector * SECTOR)).map_err(|e| e.to_string())?;
        f.read_exact(&mut table).map_err(|e| format!("reading the disc's file table: {e}"))?;
        let mut files = Vec::new();
        let mut pending = vec![0usize];
        let mut seen = std::collections::HashSet::new();
        while let Some(at) = pending.pop() {
            if at + 14 > table.len() || !seen.insert(at) {
                continue;
            }
            let u16_at = |o: usize| u16::from_le_bytes([table[o], table[o + 1]]);
            let u32_at = |o: usize| u32::from_le_bytes(table[o..o + 4].try_into().unwrap());
            let (left, right) = (u16_at(at), u16_at(at + 2));
            if left == 0xFFFF {
                continue; // padding at the end of a sector
            }
            let (start, length, attrs, name_len) = (u32_at(at + 4), u32_at(at + 8), table[at + 12], table[at + 13]);
            let name_end = (at + 14 + name_len as usize).min(table.len());
            let name = String::from_utf8_lossy(&table[at + 14..name_end]).to_string();
            if left != 0 {
                pending.push(left as usize * 4);
            }
            if right != 0 {
                pending.push(right as usize * 4);
            }
            if attrs & 0x10 == 0 {
                files.push((name, base + start as u64 * SECTOR, length as u64));
            }
        }
        return Ok(files);
    }
    Err("this file is not an Xbox disc image (an ISO of the game disc, or the folder with its files)".into())
}

/// default.xbe's bytes, and whether ige.xbe (the map editor) is there too
fn read_executable(path: &Path) -> Result<(Vec<u8>, bool), String> {
    if path.is_dir() {
        let mut xbe = None;
        let mut editor = false;
        for entry in std::fs::read_dir(path).map_err(|e| e.to_string())?.flatten() {
            let name = entry.file_name().to_string_lossy().to_lowercase();
            if name == "default.xbe" {
                xbe = Some(entry.path());
            }
            editor |= name == "ige.xbe";
        }
        let xbe = xbe.ok_or("this folder has no default.xbe: choose the folder with the disc's files, or the disc image")?;
        return Ok((std::fs::read(xbe).map_err(|e| e.to_string())?, editor));
    }
    let mut f = File::open(path).map_err(|e| format!("can't open it: {e}"))?;
    let files = root_files(&mut f)?;
    let editor = files.iter().any(|(n, ..)| n.eq_ignore_ascii_case("ige.xbe"));
    let (_, at, size) = files
        .iter()
        .find(|(n, ..)| n.eq_ignore_ascii_case("default.xbe"))
        .ok_or("this disc image has no default.xbe: it is not a game disc")?;
    let mut data = vec![0u8; (*size).min(64 << 20) as usize];
    f.seek(SeekFrom::Start(*at)).map_err(|e| e.to_string())?;
    f.read_exact(&mut data).map_err(|e| format!("reading default.xbe: {e}"))?;
    Ok((data, editor))
}

pub struct Certificate {
    pub title_id: String,
    pub name: String,
    pub version: u64,
    pub region: String,
}

pub fn certificate(xbe: &[u8]) -> Option<Certificate> {
    let u32_at = |o: usize| xbe.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    if xbe.get(..4)? != b"XBEH" {
        return None;
    }
    let at = u32_at(0x118)?.checked_sub(u32_at(0x104)?)? as usize;
    let region = u32_at(at + 0xA0)?;
    let raw: Vec<u16> = xbe.get(at + 0x0C..at + 0x0C + 80)?.chunks(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
    let name = String::from_utf16_lossy(&raw).split('\0').next().unwrap_or("").trim().to_string();
    let regions: Vec<&str> = [(1, "North America"), (2, "Japan"), (4, "PAL: Europe and Australia")]
        .iter()
        .filter(|(bit, _)| region & bit != 0)
        .map(|(_, n)| *n)
        .collect();
    Some(Certificate {
        title_id: format!("{:08X}", u32_at(at + 0x08)?),
        name,
        version: u32_at(at + 0xAC)? as u64,
        region: if regions.is_empty() { format!("region 0x{region:08X}") } else { regions.join(", ") },
    })
}

pub fn check(path: &Path, source: Option<&Path>) -> DiscCheck {
    let mut out = DiscCheck { path: path.to_string_lossy().to_string(), ..Default::default() };
    let (xbe, editor) = match read_executable(path) {
        Ok(found) => found,
        Err(e) => {
            out.headline = "Not a game disc".into();
            out.message = capitalise(&e);
            return out;
        }
    };
    let digest = format!("{:x}", Sha256::digest(&xbe));
    let cert = certificate(&xbe);
    if let Some(c) = &cert {
        out.detail = format!("Title {} · version {} · {}", c.title_id, c.version, c.region);
    }
    let matching: Option<&Title> = TITLES.iter().find(|t| t.release(source).default_xbe == digest);
    if let Some(title) = matching {
        out.ok = true;
        out.title = Some(title.key.into());
        out.headline = format!("{} · supported disc", title.name);
        out.message = if editor {
            "Ready to build. The map editor can be built from this disc too.".into()
        } else {
            "Ready to build.".into()
        };
        return out;
    }
    let Some(c) = cert else {
        out.headline = "Not a supported disc".into();
        out.message = "Its default.xbe is unreadable. Use a complete, unmodified dump of the disc.".into();
        return out;
    };
    match TITLES.iter().find(|t| t.title_id.eq_ignore_ascii_case(&c.title_id)) {
        Some(title) => {
            out.title = Some(title.key.into());
            let supported = title.release(source).version;
            if c.version != supported {
                out.headline = format!("{} · version {} is not supported yet", title.name, c.version);
                out.message = format!(
                    "This is version {} ({}). Only version {supported} can be built for now.",
                    c.version, c.region
                );
            } else {
                out.headline = format!("{} · modified disc", title.name);
                out.message = "Its default.xbe was changed (a patched XISO, for example). Use an unmodified dump of the disc.".into();
            }
        }
        None => {
            out.headline = "Not Far Cry Instincts or Evolution".into();
            let name = if c.name.is_empty() { "another game".to_string() } else { c.name.clone() };
            out.message = format!("This disc is {name} (title {}).", c.title_id);
        }
    }
    out
}

fn capitalise(s: &str) -> String {
    let mut c = s.chars();
    c.next().map(|f| f.to_uppercase().collect::<String>() + c.as_str()).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn xbe(title_id: u32, version: u32, region: u32) -> Vec<u8> {
        let mut h = vec![0u8; 0x1000];
        h[..4].copy_from_slice(b"XBEH");
        h[0x104..0x108].copy_from_slice(&0x10000u32.to_le_bytes());
        h[0x118..0x11C].copy_from_slice(&0x10200u32.to_le_bytes());
        h[0x208..0x20C].copy_from_slice(&title_id.to_le_bytes());
        for (i, c) in "Far Cry".encode_utf16().enumerate() {
            h[0x20C + 2 * i..0x20E + 2 * i].copy_from_slice(&c.to_le_bytes());
        }
        h[0x2A0..0x2A4].copy_from_slice(&region.to_le_bytes());
        h[0x2AC..0x2B0].copy_from_slice(&version.to_le_bytes());
        h
    }

    /// an XISO with the files in its root folder
    fn image(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
        let mut table = Vec::new();
        let mut offsets = Vec::new();
        for (name, _) in files {
            offsets.push(table.len());
            table.resize(table.len() + ((14 + name.len() + 3) & !3), 0xFF);
        }
        let mut data_sector = 40u32;
        let mut img = vec![0u8; 40 * SECTOR as usize];
        for (i, (name, content)) in files.iter().enumerate() {
            let right = if i + 1 < files.len() { (offsets[i + 1] / 4) as u16 } else { 0 };
            let at = offsets[i];
            table[at..at + 2].copy_from_slice(&0u16.to_le_bytes());
            table[at + 2..at + 4].copy_from_slice(&right.to_le_bytes());
            table[at + 4..at + 8].copy_from_slice(&data_sector.to_le_bytes());
            table[at + 8..at + 12].copy_from_slice(&(content.len() as u32).to_le_bytes());
            table[at + 12] = 0x20;
            table[at + 13] = name.len() as u8;
            table[at + 14..at + 14 + name.len()].copy_from_slice(name.as_bytes());
            img.extend_from_slice(content);
            img.resize(img.len().div_ceil(SECTOR as usize) * SECTOR as usize, 0);
            data_sector = (img.len() / SECTOR as usize) as u32;
        }
        let head = 32 * SECTOR as usize;
        img[head..head + 20].copy_from_slice(MAGIC);
        img[head + 0x14..head + 0x18].copy_from_slice(&33u32.to_le_bytes());
        img[head + 0x18..head + 0x1C].copy_from_slice(&(table.len() as u32).to_le_bytes());
        img[head + 0x7EC..head + 0x7EC + 20].copy_from_slice(MAGIC);
        let t = 33 * SECTOR as usize;
        img[t..t + table.len()].copy_from_slice(&table);
        img
    }

    fn check_image(files: &[(&str, Vec<u8>)]) -> DiscCheck {
        let dir = std::env::temp_dir().join(format!("fci-disc-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join(format!("{}.iso", files.len() + files[0].1.len()));
        std::fs::write(&path, image(files)).unwrap();
        let found = check(&path, None);
        std::fs::remove_file(path).unwrap();
        found
    }

    #[test]
    fn a_pal_disc_says_its_version() {
        let c = check_image(&[("default.xbe", xbe(0x5553_0008, 4, 4)), ("ige.xbe", vec![1])]);
        assert!(!c.ok);
        assert_eq!(c.title.as_deref(), Some("instincts"));
        assert!(c.message.contains("version 4 (PAL: Europe and Australia)"), "{}", c.message);
    }

    #[test]
    fn a_patched_disc_is_called_modified() {
        let c = check_image(&[("default.xbe", xbe(0x5553_0060, 3, 1))]);
        assert!(c.headline.contains("modified"), "{}", c.headline);
    }

    #[test]
    fn another_game_is_named() {
        let c = check_image(&[("a.txt", vec![0; 10]), ("default.xbe", xbe(0x4D53_0004, 1, 1))]);
        assert!(c.message.contains("title 4D530004"), "{}", c.message);
    }

    #[test]
    fn not_a_disc() {
        let dir = std::env::temp_dir().join(format!("fci-disc-test-not-{}", std::process::id()));
        std::fs::write(&dir, vec![0u8; 100_000]).unwrap();
        let c = check(&dir, None);
        std::fs::remove_file(dir).unwrap();
        assert!(!c.ok && c.message.starts_with("This file is not an Xbox disc image"), "{}", c.message);
    }
}
