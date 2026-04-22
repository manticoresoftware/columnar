use rayon::prelude::*;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Instant;

fn main() {
    let ncpus = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4);
    println!("CPUs: {}", ncpus);
    println!("rayon threads: {}", rayon::current_num_threads());

    // Create a dummy ONNX model (just a single Add node)
    // We test if multiple ORT sessions with intra_threads=1 can Run() truly in parallel

    // Build a trivial ONNX-like workload: create N sessions, dispatch via rayon
    let intra = 1usize;
    let num_sessions = ncpus / intra;
    println!(
        "Creating {} sessions with intra_threads={}",
        num_sessions, intra
    );

    let mut sessions: Vec<Mutex<ort::session::Session>> = Vec::new();

    // We need a model file. Create a minimal one in memory using the ort API.
    // Actually, let's just test with random tensor computation to simulate the pattern.
    // The real question is whether Mutex<Session> + rayon works, which we proved with sleep.
    //
    // Instead, test with actual ORT sessions if we can find/create a model.
    // For now, test the exact pattern with heavier CPU work (matrix multiply simulation).

    println!("\n=== Testing rayon + mutex pool with CPU-bound work ===");
    for &num_sess in &[1, 2, 4, ncpus] {
        let mutexes: Vec<Mutex<Vec<f32>>> = (0..num_sess)
            .map(|_| Mutex::new(vec![0.0f32; 384 * 512]))
            .collect();
        let active = AtomicUsize::new(0);
        let max_active = AtomicUsize::new(0);
        let num_batches = num_sess * 4;

        let start = Instant::now();
        let _: Vec<f32> = (0..num_batches)
            .into_par_iter()
            .enumerate()
            .map(|(i, _)| {
                let cur = active.fetch_add(1, Ordering::Relaxed) + 1;
                max_active.fetch_max(cur, Ordering::Relaxed);
                let mut guard = mutexes[i % num_sess].lock().unwrap();

                // Simulate real CPU work: dot products like ONNX would do
                let mut sum = 0.0f32;
                for j in 0..guard.len() {
                    guard[j] = (j as f32 * 0.001).sin();
                    sum += guard[j] * guard[j];
                }

                active.fetch_sub(1, Ordering::Relaxed);
                sum
            })
            .collect();
        let elapsed = start.elapsed();

        println!(
            "sessions={:3}  batches={:3}  max_concurrent={:3}  took={:6.0}ms  speedup={:.1}x",
            num_sess,
            num_batches,
            max_active.load(Ordering::Relaxed),
            elapsed.as_millis(),
            if num_sess == 1 {
                1.0
            } else {
                // compare to sessions=1 baseline by ratio
                elapsed.as_millis() as f64 / elapsed.as_millis() as f64
            }
        );
    }

    // Now test with actual ORT if possible
    println!("\n=== Testing actual ORT sessions ===");

    // Try to create ORT sessions with a minimal model
    match create_minimal_ort_sessions(ncpus) {
        Ok(results) => {
            for r in results {
                println!("{}", r);
            }
        }
        Err(e) => println!("ORT test skipped: {}", e),
    }
}

fn create_minimal_ort_sessions(
    ncpus: usize,
) -> Result<Vec<String>, Box<dyn std::error::Error>> {
    // We can't easily create a model without a file, so test session creation timing
    // to see if multiple sessions share state

    let mut results = Vec::new();

    // Just verify we can create sessions with different intra_threads
    for intra in [1, 2, 4] {
        let start = Instant::now();
        let builder = ort::session::Session::builder()?
            .with_intra_threads(intra)?;
        let elapsed = start.elapsed();
        results.push(format!(
            "Session::builder with intra_threads={}: {:?} (builder created, no model to load)",
            intra, elapsed
        ));
        drop(builder);
    }

    Ok(results)
}
