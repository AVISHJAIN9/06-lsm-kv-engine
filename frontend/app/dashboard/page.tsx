export default function DashboardPage() {
  return (
    <div>
      <h1 className="text-3xl font-bold text-white mb-6">LSM-Tree Key-Value Engine — Live Internals Dashboard</h1>
      <div className="grid grid-cols-1 md:grid-cols-3 gap-6">
        <div className="p-6 rounded-xl bg-slate-900 border border-slate-800">
          <div className="text-slate-400 text-xs">SYSTEM THROUGHPUT</div>
          <div className="text-2xl font-bold text-cyan-400">TO BE MEASURED</div>
        </div>
        <div className="p-6 rounded-xl bg-slate-900 border border-slate-800">
          <div className="text-slate-400 text-xs">P99 LATENCY</div>
          <div className="text-2xl font-bold text-emerald-400">TO BE MEASURED</div>
        </div>
        <div className="p-6 rounded-xl bg-slate-900 border border-slate-800">
          <div className="text-slate-400 text-xs">TIER A ANOMALY SCORE</div>
          <div className="text-2xl font-bold text-purple-400">TO BE MEASURED</div>
        </div>
      </div>
    </div>
  )
}
