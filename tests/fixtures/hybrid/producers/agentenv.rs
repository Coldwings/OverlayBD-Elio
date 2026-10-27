// Reference implementation harness: ordinary files in a fresh private directory.
use anyhow::{ensure, Result};
use std::{path::Path, sync::Arc};
use overlaybd::{backend::local::LocalFile, index::{LogIndex, Segment, SegmentMapping}, index_file::{create_file_rw, open_file_ro, open_file_rw, LayerInfo, RwLayout, LSMTFile}, virtual_file::VirtualFile};
const OPS: [(u64, usize, u8, bool); 8] = [(1,2,0x11,false),(8,3,0x22,false),(2,1,0x33,false),(9,1,0,true),(9,1,0x44,false),(10,1,0,true),(16,2,0,true),(63,1,0x77,false)];
fn oracle() -> (Vec<u8>, Vec<u8>) {
    let mut bytes = vec![0; 32768]; let mut states = vec![0; 64];
    for (sector,count,value,zero) in OPS {
        let start = sector as usize;
        bytes[start*512..(start+count)*512].fill(if zero {0} else {value});
        states[start..start+count].fill(if zero {2} else {1});
    }
    (bytes,states)
}
async fn verify(file: &dyn VirtualFile, maps: Vec<SegmentMapping>) -> Result<()> {
    let (expected, expected_states) = oracle();
    ensure!(file.read_at(0, expected.len()).await?.as_ref() == expected, "byte mismatch");
    let mut states = vec![0;64];
    for m in maps {
        ensure!(m.length() > 0 && m.end() <= 64, "mapping bounds");
        states[m.offset() as usize..m.end() as usize].fill(if m.zeroed {2} else {1});
        println!("MAP offset={} length={} moffset={} zero={} tag={}", m.offset(), m.length(), m.moffset, m.zeroed, m.tag);
    }
    ensure!(states == expected_states, "mapping state mismatch");
    Ok(())
}
async fn verify_rw(file: &LSMTFile) -> Result<()> {
    let mut maps = vec![];
    file.index().read().await.lookup(Segment::new(0,64), &mut maps);
    verify(file,maps).await
}
async fn produce(dir: &Path) -> Result<()> {
    std::fs::create_dir(dir)?;
    for (name,seal,buffered) in [("writable",false,false),("buffered",false,true),("sealed",true,false)] {
        let data = Arc::new(LocalFile::new(dir.join(format!("{name}.data")))?);
        let index = Arc::new(LocalFile::new(dir.join(format!("{name}.index")))?);
        let mut info = LayerInfo::new(data.clone(),Some(index),32768);
        info.rw_layout = RwLayout::HybridLogStructured;
        info.uuid = uuid::Uuid::parse_str("11111111-2222-4333-8444-555555555555")?;
        info.user_tag = Some(b"overlaybd-elio-hybrid-interop-v1".to_vec());
        let file = create_file_rw(info).await?;
        if buffered { file.set_index_group_commit(4096)?; }
        for (sector,count,value,zero) in OPS {
            if zero { file.discard_range(sector*512,(count*512) as u64).await?; }
            else { ensure!(file.write_at(sector*512,&vec![value;count*512]).await? == count*512, "short write"); }
        }
        file.sync().await?;
        verify_rw(&file).await?;
        if seal { file.close_seal().await?; data.sync().await?; }
    }
    let (bytes,states)=oracle();
    std::fs::write(dir.join("expected.raw"),bytes)?;
    std::fs::write(dir.join("expected.states"),states)?;
    Ok(())
}
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    ensure!(args.len() >= 3, "usage: produce DIR | check-ro DATA | check-rw DATA INDEX");
    match args[1].as_str() {
        "produce" => produce(Path::new(&args[2])).await?,
        "check-ro" => {
            let file = open_file_ro(Arc::new(LocalFile::open_ro(&args[2])?)).await?;
            let mut maps=vec![]; file.index().lookup(Segment::new(0,64),&mut maps);
            verify(&file,maps).await?;
        },
        "check-rw" => {
            ensure!(args.len() == 4, "index required");
            let file = open_file_rw(Arc::new(LocalFile::open_rw(&args[2],false)?),Some(Arc::new(LocalFile::open_rw(&args[3],false)?))).await?;
            verify_rw(&file).await?;
        },
        _ => anyhow::bail!("unknown mode"),
    }
    println!("AGENTENV_HYBRID_FIXTURE_OK {}",args[1]);
    Ok(())
}
