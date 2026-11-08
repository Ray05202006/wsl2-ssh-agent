// Measurement prototype only: the Windows helper (one connection) in Rust, std only.
use std::fs::OpenOptions;
use std::io::{Read, Write};

fn read_msg(r: &mut impl Read, buf: &mut [u8]) -> Option<usize> {
    r.read_exact(&mut buf[..4]).ok()?;
    let n = u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) as usize;
    if n == 0 || n > 256 * 1024 { return None; }
    r.read_exact(&mut buf[4..4 + n]).ok()?;
    Some(n + 4)
}

fn main() {
    let mut pipe = match OpenOptions::new().read(true).write(true).open(r"\\.\pipe\openssh-ssh-agent") {
        Ok(p) => p,
        Err(_) => std::process::exit(2),
    };
    let mut buf = vec![0u8; 4 + 256 * 1024];
    let (mut i, mut o) = (std::io::stdin().lock(), std::io::stdout().lock());
    while let Some(n) = read_msg(&mut i, &mut buf) {
        if pipe.write_all(&buf[..n]).is_err() { std::process::exit(3) }
        let Some(n) = read_msg(&mut pipe, &mut buf) else { std::process::exit(3) };
        if o.write_all(&buf[..n]).and_then(|_| o.flush()).is_err() { break }
    }
}
