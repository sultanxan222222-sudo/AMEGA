using System.Diagnostics;
using System.Net;
using System.Reflection;
using System.Windows.Forms;

internal static class Program
{
    [STAThread]
    private static int Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        Type mainFormType = Assembly.Load("AMEGA").GetType(
            "AMEGA.Windows.MainForm", throwOnError: true)!;

        using var form = (Form)Activator.CreateInstance(mainFormType, nonPublic: true)!;
        form.Show();
        Application.DoEvents();

        Button search = Field<Button>(form, "_startButton");
        Button stop = Field<Button>(form, "_stopButton");

        Require(search.Enabled, "Search must be enabled on first launch without choosing a PKG.");
        Require(!stop.Enabled, "Stop must be disabled before search starts.");

        RunSearchCycle(form, search, stop, 1);
        RunSearchCycle(form, search, stop, 2);

        form.Close();
        Application.DoEvents();

        Console.WriteLine("AMEGA WinForms Search/Stop/Restart integration test passed.");
        return 0;
    }

    private static void RunSearchCycle(Form form, Button search, Button stop, int cycle)
    {
        Click(form, search, $"Search button must be enabled before cycle {cycle}.");

        WaitUntil(form, () => Field<bool>(form, "_isSearching"), TimeSpan.FromSeconds(20),
            $"Search did not start in cycle {cycle}.");

        Require(!search.Enabled, $"Search must disable while discovery is running (cycle {cycle}).");
        Require(stop.Enabled, $"Stop must enable while discovery is running (cycle {cycle}).");
        Require(stop.Text == "Остановить поиск",
            $"Stop button must show its active-search label after startup (cycle {cycle}); actual: {stop.Text}");

        Click(form, stop, $"Stop button must be enabled in cycle {cycle}.");

        WaitUntil(form,
            () => !Field<bool>(form, "_isSearching") && search.Enabled && !stop.Enabled,
            TimeSpan.FromSeconds(20),
            $"Stop did not return the UI to its idle state in cycle {cycle}.");
    }

    private static void Click(Form form, Button button, string message)
    {
        Require(button.Enabled, message);
        button.PerformClick();
        Application.DoEvents();
    }

    private static void WaitUntil(Form form, Func<bool> condition, TimeSpan timeout, string message)
    {
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < timeout)
        {
            Application.DoEvents();
            if (condition()) return;
            Thread.Sleep(10);
        }

        throw new TimeoutException(message);
    }

    private static T Field<T>(object target, string name)
    {
        FieldInfo field = target.GetType().GetField(
            name, BindingFlags.Instance | BindingFlags.NonPublic)
            ?? throw new InvalidOperationException($"Private test field '{name}' was not found.");
        return (T)(field.GetValue(target)
            ?? throw new InvalidOperationException($"Private test field '{name}' was null."));
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
