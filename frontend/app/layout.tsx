export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" className="dark">
      <body className="bg-slate-950 text-slate-100 min-h-screen antialiased">
        <header className="h-16 border-b border-slate-800 flex items-center justify-between px-6">
          <span className="font-bold text-lg text-white">LSM-Tree Key-Value Engine</span>
          <nav className="flex gap-4 text-sm">
            <a href="/dashboard" className="text-slate-300 hover:text-cyan-400">Dashboard</a>
            <a href="/ai-insights" className="text-slate-300 hover:text-cyan-400">AI Diagnostics</a>
          </nav>
        </header>
        <main className="p-8">{children}</main>
      </body>
    </html>
  )
}
