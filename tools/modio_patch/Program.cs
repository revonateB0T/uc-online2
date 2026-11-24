// Embed a mod.io access token into Space Engineers' mod.io assembly.
//
// The game authenticates against mod.io with a Steam encrypted app ticket
// (POST /v1/external/steamauth).  A ticket that was not minted for app 244850
// is refused with HTTP 401 / error_ref 11018, so the in-game mod browser never
// gets a session.
//
// This patch replaces the body of MyModIo.AuthenticateWithTicket() -- the
// method that turns the ticket into an access token -- with a version that
// simply uses the token you already have.  Everything else (listing mods,
// downloading, ratings) then talks to mod.io as that token.
//
// The original file is kept next to the patched one as <name>.orig, and the
// patch is always applied to that original, so it can be re-run after the
// token was renewed.
//
// Usage:
//   dotnet run -- --dll "/path/to/VRage.Mod.Io.dll" --token-file ~/.uc-online2-modio/token.json
//   dotnet run -- --dll "/path/to/VRage.Mod.Io.dll" --restore

using System.Text.Json;
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Program
{
    static int Main(string[] args)
    {
        string dll = null, tokenFile = null;
        bool restore = false;
        var searchDirs = new List<string>();

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--dll": dll = args[++i]; break;
                case "--token-file": tokenFile = args[++i]; break;
                case "--restore": restore = true; break;
                case "--search-dir": searchDirs.Add(args[++i]); break;
                case "--help":
                case "-h":
                    Console.WriteLine("usage: modio_patch --dll <VRage.Mod.Io.dll> [--token-file <token.json>] [--restore]");
                    return 0;
            }
        }

        if (dll == null || !File.Exists(dll))
        {
            Console.Error.WriteLine("the assembly was not found: " + (dll ?? "(no --dll given)"));
            return 1;
        }

        string dir = Path.GetDirectoryName(Path.GetFullPath(dll));
        string original = Path.Combine(dir, Path.GetFileName(dll) + ".orig");

        if (restore)
        {
            if (!File.Exists(original)) { Console.Error.WriteLine("no backup at " + original); return 1; }
            File.Copy(original, dll, true);
            Console.WriteLine("restored " + dll + " from " + original);
            return 0;
        }

        // Always start from the untouched file, so the patch can be re-applied.
        if (!File.Exists(original))
        {
            File.Copy(dll, original);
            Console.WriteLine("kept the original as " + original);
        }

        string accessToken;
        int expires = (int)DateTimeOffset.UtcNow.AddYears(1).ToUnixTimeSeconds();
        if (tokenFile != null)
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(tokenFile));
            var root = doc.RootElement;
            accessToken = root.GetProperty("access_token").GetString();
            if (root.TryGetProperty("date_expires", out var exp))
                expires = exp.GetInt32();
        }
        else
        {
            accessToken = Environment.GetEnvironmentVariable("MODIO_ACCESS_TOKEN");
            if (string.IsNullOrEmpty(accessToken))
            {
                Console.Error.WriteLine("no token: pass --token-file or set MODIO_ACCESS_TOKEN");
                return 1;
            }
        }

        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(dir);
        foreach (var extra in searchDirs) resolver.AddSearchDirectory(extra);
        var parameters = new ReaderParameters { AssemblyResolver = resolver, ReadWrite = false, InMemory = true };

        using var assembly = AssemblyDefinition.ReadAssembly(original, parameters);

        var modIo = assembly.MainModule.GetType("VRage.Mod.Io.MyModIo");
        if (modIo == null)
        {
            Console.Error.WriteLine("VRage.Mod.Io.MyModIo not found -- wrong file?");
            return 1;
        }

        var method = modIo.Methods.FirstOrDefault(m => m.Name == "AuthenticateWithTicket");
        if (method == null)
        {
            Console.Error.WriteLine("AuthenticateWithTicket not found");
            return 1;
        }

        var tokenType = assembly.MainModule.GetType("VRage.Mod.Io.Data.AccessToken")
                        ?? assembly.MainModule.Types.FirstOrDefault(t => t.Name == "AccessToken");
        var tokenField = modIo.Fields.FirstOrDefault(f => f.Name == "m_authenticatedToken");
        var serviceField = modIo.Fields.FirstOrDefault(f => f.Name == "m_service");
        var onAuthenticated = modIo.Methods.FirstOrDefault(m => m.Name == "OnAuthenticated");
        if (tokenType == null || tokenField == null || serviceField == null || onAuthenticated == null)
        {
            Console.Error.WriteLine($"unexpected shape: tokenType={tokenType != null} tokenField={tokenField != null} serviceField={serviceField != null} onAuthenticated={onAuthenticated != null}");
            Console.Error.WriteLine("types: " + string.Join(", ", assembly.MainModule.Types.Select(t => t.FullName).Where(n => n.Contains("AccessToken") || n.Contains("MyModIo"))));
            Console.Error.WriteLine("methods: " + string.Join(", ", modIo.Methods.Select(m => m.Name)));
            return 1;
        }

        // The result enum lives in another assembly; take the value of its OK member.
        var resultType = onAuthenticated.Parameters[3].ParameterType;
        var ok = resultType.Resolve().Fields.FirstOrDefault(f => f.Name == "OK");
        if (ok == null || !ok.HasConstant)
        {
            Console.Error.WriteLine("could not find MyGameServiceCallResult.OK");
            return 1;
        }

        var userIdGetter = serviceField.FieldType.Resolve()
            .Methods.FirstOrDefault(m => m.Name == "get_UserId");
        if (userIdGetter == null)
        {
            Console.Error.WriteLine("could not find IMyGameService.UserId");
            return 1;
        }

        var accessTokenField = tokenType.Fields.First(f => f.Name == "access_token");
        var expiresField = tokenType.Fields.First(f => f.Name == "date_expires");

        if (!method.HasBody) { Console.Error.WriteLine("method has no body"); return 1; }

        method.Body.Instructions.Clear();
        method.Body.ExceptionHandlers.Clear();
        method.Body.Variables.Clear();
        var il = method.Body.GetILProcessor();

        // m_authenticatedToken = new AccessToken { access_token = <token>, date_expires = <exp> };
        il.Emit(OpCodes.Newobj, tokenType.Methods.First(m => m.IsConstructor && m.Parameters.Count == 0));
        il.Emit(OpCodes.Dup);
        il.Emit(OpCodes.Ldstr, accessToken);
        il.Emit(OpCodes.Stfld, accessTokenField);
        il.Emit(OpCodes.Dup);
        il.Emit(OpCodes.Ldc_I4, expires);
        il.Emit(OpCodes.Stfld, expiresField);
        il.Emit(OpCodes.Stsfld, tokenField);

        // OnAuthenticated(m_service.UserId, share, m_authenticatedToken, OK, onDone);
        var importedUserId = assembly.MainModule.ImportReference(userIdGetter);
        il.Emit(OpCodes.Ldsfld, serviceField);
        il.Emit(OpCodes.Callvirt, importedUserId);
        il.Emit(OpCodes.Ldarg, method.Parameters[2]);   // share
        il.Emit(OpCodes.Ldsfld, tokenField);
        il.Emit(OpCodes.Ldc_I4, Convert.ToInt32(ok.Constant));
        il.Emit(OpCodes.Ldarg, method.Parameters[3]);   // onDone
        il.Emit(OpCodes.Call, onAuthenticated);
        il.Emit(OpCodes.Ret);

        // Write next to the target and move it into place, so a failure never
        // leaves a half-written assembly behind.
        string tmp = dll + ".new";
        assembly.Write(tmp);
        assembly.Dispose();
        File.Move(tmp, dll, true);
        Console.WriteLine("patched " + dll);
        Console.WriteLine("  the Steam ticket is ignored now, mod.io is called as the token's user");
        Console.WriteLine("  token length " + accessToken.Length + ", expires " + expires);
        return 0;
    }
}
