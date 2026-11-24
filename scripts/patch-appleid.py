# Applies nekodrop's changes to WirDrop's appleid crate before it is built.
# Usage: python patch-appleid.py <third_party\WirDrop>
#
# - Phone numbers written without a country code: WirDrop takes them as Korean
#   (+82). nekodrop's users are mostly in mainland China, Taiwan and Hong Kong,
#   so the country code comes from APPLEID_CC (the client sets it from the
#   Windows region), +86 when unset, and an 11-digit mainland mobile number
#   (1xxxxxxxxxx) is always +86.
import pathlib
import sys

path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'contacts.rs'
text = path.read_text(encoding='utf-8')
MARK = 'nekodrop: APPLEID_CC'
done = MARK in text

old = '''    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+82{rest}")
    } else {
        format!("+{digits}")
    };'''
new = '''    } else if digits.len() == 11 && digits.starts_with('1') {
        // nekodrop: APPLEID_CC. A mainland mobile number written without +86.
        format!("+86{digits}")
    } else if let Some(rest) = digits.strip_prefix('0') {
        format!("+{}{rest}", country_code())
    } else {
        format!("+{digits}")
    };'''
if not done and text.count(old) != 1:
    sys.exit('contacts.rs: phone_spellings moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = '''    if let Some(rest) = e164.strip_prefix("+82") {
        out.push(("national", format!("0{rest}")));
    }'''
new = '''    let cc = format!("+{}", country_code());
    if let Some(rest) = e164.strip_prefix(cc.as_str()) {
        out.push(("national", format!("0{rest}")));
    }
    if let Some(rest) = e164.strip_prefix("+86") {
        out.push(("national", rest.to_string()));
    }'''
if not done and text.count(old) != 1:
    sys.exit('contacts.rs: the national spelling moved; update patch-appleid.py')
text = text.replace(old, new, 1)

old = 'fn email_spellings('
new = '''/// The country of numbers written without one (nekodrop: APPLEID_CC, else 86).
fn country_code() -> String {
    std::env::var("APPLEID_CC")
        .ok()
        .filter(|c| !c.is_empty() && c.chars().all(|ch| ch.is_ascii_digit()))
        .unwrap_or_else(|| "86".to_string())
}

fn email_spellings('''
if not done:
    text = text.replace(old, new, 1)
    # WirDrop's own test pins the Korean default; nekodrop's default differs.
    text = text.replace('fn spells_phone_numbers_every_way() {', '#[ignore = "nekodrop: default country is not Korea"]\n    fn spells_phone_numbers_every_way() {', 1)
    path.write_text(text, encoding='utf-8')
    print('appleid: phone numbers without a country code follow APPLEID_CC (default +86)')

# anisette.rs: the first sign-in downloads iCloud for Windows 7.21 (about 160 MB)
# from one Apple URL, in one piece, through the sign-in client, whose 120 s
# limit covers the whole request: below about 1.3 MB/s the download always
# failed, and nothing showed how far it had got (#42).  Instead: try several
# sources in turn (Apple's two CDNs, then the Internet Archive's copy), each
# streamed with a 60 s limit between reads, each checked against the same
# SHA-256, and report the progress ("progress" events with --json).  The file
# is still downloaded from these sources on first sign-in, never shipped.
path = pathlib.Path(sys.argv[1]) / 'crates' / 'appleid' / 'src' / 'anisette.rs'
text = path.read_text(encoding='utf-8')
MARK = 'nekodrop: download sources'
if MARK not in text:
    old = """    let setup = crate::http::client()
        .get(ICLOUD_SETUP)
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| e.to_string())?
        .bytes()
        .await
        .map_err(|e| e.to_string())?;
    let digest: String = Sha256::digest(&setup).iter().map(|b| format!("{b:02x}")).collect();
    if digest != ICLOUD_SETUP_SHA256 {
        return Err("iCloudSetup.exe isn't the one expected".into());
    }
"""
    new = """    let setup = fetch_icloud_setup().await?; // nekodrop: download sources (#42)
"""
    if text.count(old) != 1:
        sys.exit('anisette.rs: the iCloudSetup download moved; update patch-appleid.py')
    text = text.replace(old, new, 1)
    old = 'async fn unpack_icloud(home: &Path) -> Result<(), String> {'
    new = """/// nekodrop (#42): where iCloudSetup.exe 7.21 can be had, tried in order.
/// Every copy must match ICLOUD_SETUP_SHA256, so a source cannot change it.
const ICLOUD_SETUP_SOURCES: [&str; 3] = [
    ICLOUD_SETUP,
    "http://appldnld.apple.com/windows/061-91601-20200323-974a39d0-41fc-4761-b571-318b7d9205ed/iCloudSetup.exe",
    "https://web.archive.org/web/20210228113943id_/https://secure-appldnld.apple.com/windows/061-91601-20200323-974a39d0-41fc-4761-b571-318b7d9205ed/iCloudSetup.exe",
];

/// How much of iCloudSetup.exe has arrived (nekodrop, #42).
fn download_progress(got: u64, total: u64) {
    const MB: u64 = 1024 * 1024;
    if crate::JSON.load(std::sync::atomic::Ordering::Relaxed) {
        crate::event(serde_json::json!({ "ev": "progress", "done": got, "total": total }));
    } else if total > 0 {
        println!("  {}% ({} of {} MB)", got * 100 / total, got / MB, total / MB);
    }
}

/// One source, streamed: no limit on the whole download, 60 s between reads.
async fn fetch_from(url: &str) -> Result<Vec<u8>, String> {
    let mut response = reqwest::Client::builder()
        .connect_timeout(Duration::from_secs(30))
        .read_timeout(Duration::from_secs(60))
        .build()
        .map_err(|e| e.to_string())?
        .get(url)
        .send()
        .await
        .and_then(|r| r.error_for_status())
        .map_err(|e| e.to_string())?;
    let total = response.content_length().unwrap_or(0);
    let mut setup: Vec<u8> = Vec::with_capacity(total as usize);
    let mut shown = u64::MAX;
    while let Some(chunk) = response.chunk().await.map_err(|e| e.to_string())? {
        setup.extend_from_slice(&chunk);
        let got = setup.len() as u64;
        let step = if total > 0 { got * 100 / total } else { got >> 23 };
        if step != shown {
            shown = step;
            download_progress(got, total);
        }
    }
    let digest: String = Sha256::digest(&setup).iter().map(|b| format!("{b:02x}")).collect();
    if digest != ICLOUD_SETUP_SHA256 {
        return Err("the download isn't the iCloudSetup.exe expected".into());
    }
    Ok(setup)
}

/// The first source that delivers the expected file.
async fn fetch_icloud_setup() -> Result<Vec<u8>, String> {
    let mut why = Vec::new();
    for (n, url) in ICLOUD_SETUP_SOURCES.iter().enumerate() {
        let host = url.split('/').nth(2).unwrap_or(url);
        if n > 0 {
            say!("trying another source for iCloud for Windows 7.21 ({host})");
        }
        match fetch_from(url).await {
            Ok(setup) => return Ok(setup),
            Err(e) => {
                crate::log_line(&format!("iCloudSetup.exe from {host}: {e}"));
                why.push(format!("{host}: {e}"));
            }
        }
    }
    Err(format!("iCloud for Windows 7.21 could not be downloaded ({})", why.join("; ")))
}

async fn unpack_icloud(home: &Path) -> Result<(), String> {"""
    if text.count(old) != 1:
        sys.exit('anisette.rs: unpack_icloud moved; update patch-appleid.py')
    text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8')
    print('appleid: iCloud download from three sources, streamed with progress, SHA-256 checked')
