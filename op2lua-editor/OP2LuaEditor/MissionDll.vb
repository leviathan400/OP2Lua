' MissionDll.vb
'
' A mission's properties and the mission DLL built from them. OP2 reads a mission's name (LevelDesc), map (MapName),
' tech tree (TechtreeName), type, player count, maximum tech level and unit-only flag (DescBlock) and its multiplayer
' AI players (DescBlockEx) from the DLL before any Lua runs. The placement's header holds the same values (name, map,
' tech, type, max_tech, players_count, unit_only, ai_players) for the editor, and Save builds the DLL from them: a
' copy of the bundled stub with each value written at its export, so the DLL always says what the editor shows.

Imports System.Collections
Imports System.Collections.Generic
Imports System.IO
Imports System.Text
Imports System.Text.RegularExpressions

' The values a mission DLL describes, as the placement's header writes them. TypeName is a placement type name
' (Colony, MultiLastOneStanding, ...) or a campaign mission's number ("1").
Public Class MissionProps
    Public Name As String = ""
    Public MapFile As String = "eden01.map"
    Public Tech As String = "MULTITEK.TXT"
    Public TypeName As String = "Colony"
    Public MaxTech As Integer = 12
    Public Players As Integer = 2
    Public UnitOnly As Boolean = False
    Public AiPlayers As Integer = 0

    Public Function Clone() As MissionProps
        Return DirectCast(MemberwiseClone(), MissionProps)
    End Function
End Class

Public Module MissionDll

    ' The size of the LevelDesc, MapName and TechtreeName exports (char[1024]).
    Public Const TextExportLen As Integer = 1024

    ' The stub's FileDescription placeholder (UTF-16 in the version resource). Must stay byte-for-byte identical to
    ' the FileDescription in stub/LuaMission.rc and OP2LUA_DESC_PLACEHOLDER in the command-line tools.
    Public Const StubDescPlaceholder As String = "OP2Lua mission name placeholder - set by the new-mission tool"

    ' ===== mission types =====

    ' The placement name of a DescBlock MissionType (a campaign mission's number as text), or Nothing.
    Public Function MissionTypeName(value As Integer) As String
        If value >= 1 AndAlso value <= 12 Then Return value.ToString()
        Select Case value
            Case -1 : Return "Colony"
            Case -2 : Return "AutoDemo"
            Case -3 : Return "Tutorial"
            Case -4 : Return "MultiLandRush"
            Case -5 : Return "MultiSpaceRace"
            Case -6 : Return "MultiResourceRace"
            Case -7 : Return "MultiMidas"
            Case -8 : Return "MultiLastOneStanding"
            Case Else : Return Nothing
        End Select
    End Function

    ' ===== the placement's header =====

    Private Function KeyRegex(key As String, valuePattern As String) As Regex
        Return New Regex("(?<![\w])(" & Regex.Escape(key) & "\s*=\s*)" & valuePattern)
    End Function

    Private Function HeaderString(text As String, key As String) As String
        Dim m As Match = KeyRegex(key, """([^""]*)""").Match(text)
        Return If(m.Success, m.Groups(2).Value, Nothing)
    End Function

    Private Function HeaderInt(text As String, key As String) As Integer?
        Dim m As Match = KeyRegex(key, "(-?\d+)").Match(text)
        Dim v As Integer
        If m.Success AndAlso Integer.TryParse(m.Groups(2).Value, v) Then Return v
        Return Nothing
    End Function

    Private Function HeaderBool(text As String, key As String) As Boolean?
        Dim m As Match = KeyRegex(key, "(true|false)(?![\w])").Match(text)
        If m.Success Then Return m.Groups(2).Value = "true"
        Return Nothing
    End Function

    ' The text with a header line added after the line holding "return {" (unchanged when there is none).
    Private Function InsertHeaderLine(text As String, line As String) As String
        Dim ret As Integer = text.IndexOf("return {", StringComparison.Ordinal)
        If ret < 0 Then Return text
        Dim nl As String = PlacementText.NewLineOf(text)
        Dim eol As Integer = text.IndexOf(ChrW(10), ret)
        If eol < 0 Then Return text & nl & "  " & line & nl
        Return text.Substring(0, eol + 1) & "  " & line & nl & text.Substring(eol + 1)
    End Function

    ' The properties a placement's text declares; the ones it doesn't are dll's (when given), else the defaults.
    Public Function ReadProps(text As String, dll As MissionProps) As MissionProps
        Dim d As MissionProps = If(dll, New MissionProps())
        Dim p As New MissionProps()
        p.Name = If(HeaderString(text, "name"), If(dll IsNot Nothing, d.Name, ""))
        p.MapFile = If(HeaderString(text, "map"), If(dll IsNot Nothing, d.MapFile, ""))
        Dim tech As String = HeaderString(text, "tech")
        p.Tech = If(String.IsNullOrEmpty(tech), d.Tech, tech)
        Dim typ As String = HeaderString(text, "type")
        p.TypeName = If(String.IsNullOrEmpty(typ), d.TypeName, typ)
        p.MaxTech = If(HeaderInt(text, "max_tech"), d.MaxTech)
        p.Players = If(HeaderInt(text, "players_count"), d.Players)
        p.UnitOnly = If(HeaderBool(text, "unit_only"), d.UnitOnly)
        p.AiPlayers = If(HeaderInt(text, "ai_players"), d.AiPlayers)
        Return p
    End Function

    ' The text with the properties written over the ones it declares, and added after "return {" where it doesn't
    ' (unit_only and ai_players only when they aren't off and 0).
    Public Function WriteProps(text As String, p As MissionProps) As String
        Dim t As String = text
        For Each kv In New KeyValuePair(Of String, String)() {
                New KeyValuePair(Of String, String)("name", p.Name), New KeyValuePair(Of String, String)("map", p.MapFile),
                New KeyValuePair(Of String, String)("tech", p.Tech), New KeyValuePair(Of String, String)("type", p.TypeName)}
            Dim rx As Regex = KeyRegex(kv.Key, """[^""]*""")
            If rx.IsMatch(t) Then
                t = rx.Replace(t, "${1}""" & kv.Value.Replace("$", "$$") & """", 1)
            Else
                t = InsertHeaderLine(t, kv.Key & " = """ & kv.Value & """,")
            End If
        Next
        For Each kv In New Tuple(Of String, Integer, Boolean)() {
                Tuple.Create("max_tech", p.MaxTech, True), Tuple.Create("players_count", p.Players, True),
                Tuple.Create("ai_players", p.AiPlayers, p.AiPlayers <> 0)}
            Dim rx As Regex = KeyRegex(kv.Item1, "-?\d+")
            If rx.IsMatch(t) Then
                t = rx.Replace(t, "${1}" & kv.Item2.ToString(), 1)
            ElseIf kv.Item3 Then
                t = InsertHeaderLine(t, kv.Item1 & " = " & kv.Item2 & ",")
            End If
        Next
        Dim rb As Regex = KeyRegex("unit_only", "(true|false)(?![\w])")
        If rb.IsMatch(t) Then
            t = rb.Replace(t, "${1}" & If(p.UnitOnly, "true", "false"), 1)
        ElseIf p.UnitOnly Then
            t = InsertHeaderLine(t, "unit_only = true,")
        End If
        Return t
    End Function

    ' Why the properties can't go into a DLL, or Nothing.
    Public Function Problem(p As MissionProps) As String
        If String.IsNullOrWhiteSpace(p.Name) Then Return "The name can't be empty."
        For Each v As String In New String() {p.Name, p.MapFile, p.Tech}
            For Each c As Char In If(v, "")
                If AscW(c) > 127 Then Return "The name, map and tech tree are stored in the mission DLL as plain ASCII."
            Next
            If If(v, "").Length >= TextExportLen Then Return "The name, map and tech tree must be under " & TextExportLen & " characters."
        Next
        If String.IsNullOrWhiteSpace(p.MapFile) Then Return "Choose a map."
        If String.IsNullOrWhiteSpace(p.Tech) Then Return "Name a tech tree file (MULTITEK.TXT, say)."
        If PlacementText.MissionTypeValue(p.TypeName) = 0 Then Return "The type must be a mission type OP2 has, or a campaign mission's number (1 to 12)."
        If p.Players < 1 OrElse p.Players > 6 Then Return "Players must be 1 to 6."
        If p.MaxTech < 0 OrElse p.MaxTech > 12 Then Return "The max tech level must be 0 to 12."
        If p.AiPlayers < 0 OrElse p.AiPlayers > 5 Then Return "AI players must be 0 to 5."
        Return Nothing
    End Function

    ' ===== the DLL =====

    ' The file offset of a named export's data and how many bytes of its section's file data follow it (-1 if none).
    Public Function ExportSpan(buf() As Byte, name As String, ByRef room As Integer) As Integer
        room = 0
        Dim at As Integer = PlacementText.ExportFileOffset(buf, name)
        If at < 0 Then Return -1
        Try
            Dim pe As Integer = BitConverter.ToInt32(buf, &H3C)
            Dim numSections As Integer = BitConverter.ToUInt16(buf, pe + 6)
            Dim sections As Integer = pe + 24 + BitConverter.ToUInt16(buf, pe + 20)
            For s As Integer = 0 To numSections - 1
                Dim sec As Integer = sections + s * 40
                Dim rawSize As Integer = BitConverter.ToInt32(buf, sec + 16)
                Dim rawPtr As Integer = BitConverter.ToInt32(buf, sec + 20)
                If at >= rawPtr AndAlso at < rawPtr + rawSize Then
                    room = Math.Min(rawPtr + rawSize, buf.Length) - at
                    Return at
                End If
            Next
        Catch ex As ArgumentException
        End Try
        Return -1
    End Function

    Private Function ExportString(buf() As Byte, name As String) As String
        Dim room As Integer
        Dim at As Integer = ExportSpan(buf, name, room)
        If at < 0 Then Return Nothing
        Dim n As Integer = 0
        While n < Math.Min(room, TextExportLen) AndAlso buf(at + n) <> 0
            n += 1
        End While
        Return Encoding.ASCII.GetString(buf, at, n)
    End Function

    Private Function ExportInt(buf() As Byte, name As String, index As Integer) As Integer?
        Dim room As Integer
        Dim at As Integer = ExportSpan(buf, name, room)
        If at < 0 OrElse room < 4 * (index + 1) Then Return Nothing
        Return BitConverter.ToInt32(buf, at + 4 * index)
    End Function

    ' What a mission DLL describes, or Nothing when it isn't one (no DescBlock or a missing text export).
    ' hasAiPlayers says whether it exports DescBlockEx.
    Public Function ReadDll(buf() As Byte, ByRef hasAiPlayers As Boolean) As MissionProps
        hasAiPlayers = False
        Dim t As Integer? = ExportInt(buf, "DescBlock", 0)
        Dim level As String = ExportString(buf, "LevelDesc")
        Dim map As String = ExportString(buf, "MapName")
        Dim tech As String = ExportString(buf, "TechtreeName")
        If Not t.HasValue OrElse level Is Nothing OrElse map Is Nothing OrElse tech Is Nothing Then Return Nothing
        Dim p As New MissionProps()
        p.Name = level
        p.MapFile = map
        p.Tech = tech
        p.TypeName = If(MissionTypeName(t.Value), p.TypeName)
        p.Players = If(ExportInt(buf, "DescBlock", 1), 0)
        p.MaxTech = If(ExportInt(buf, "DescBlock", 2), 0)
        p.UnitOnly = If(ExportInt(buf, "DescBlock", 3), 0) <> 0
        Dim ai As Integer? = ExportInt(buf, "DescBlockEx", 0)
        hasAiPlayers = ai.HasValue
        p.AiPlayers = If(ai, 0)
        Return p
    End Function

    ' Where a DLL's values differ from the properties, as "max tech level 12 (here 3)".
    Public Function Differences(p As MissionProps, dll As MissionProps, hasAiPlayers As Boolean) As List(Of String)
        Dim out As New List(Of String)()
        Dim diff = Sub(what As String, inDll As String, here As String)
                       If inDll <> here Then out.Add(what & " " & inDll & " (here " & here & ")")
                   End Sub
        Dim typeOf_ = Function(n As String) If(MissionTypeName(PlacementText.MissionTypeValue(n)), n)
        diff("name", """" & dll.Name & """", """" & p.Name & """")
        diff("map", dll.MapFile.ToLowerInvariant(), p.MapFile.ToLowerInvariant())
        diff("tech tree", dll.Tech.ToLowerInvariant(), p.Tech.ToLowerInvariant())
        diff("type", typeOf_(dll.TypeName), typeOf_(p.TypeName))
        diff("max tech level", dll.MaxTech.ToString(), p.MaxTech.ToString())
        diff("players", dll.Players.ToString(), p.Players.ToString())
        diff("unit-only", dll.UnitOnly.ToString().ToLowerInvariant(), p.UnitOnly.ToString().ToLowerInvariant())
        If hasAiPlayers Then diff("AI players", dll.AiPlayers.ToString(), p.AiPlayers.ToString())
        Return out
    End Function

    Private Function WriteTextExport(buf() As Byte, name As String, value As String) As Boolean
        Dim room As Integer
        Dim at As Integer = ExportSpan(buf, name, room)
        If at < 0 OrElse room < TextExportLen OrElse value.Length >= TextExportLen Then Return False
        Array.Clear(buf, at, TextExportLen)
        Dim b() As Byte = Encoding.ASCII.GetBytes(value)
        Array.Copy(b, 0, buf, at, b.Length)
        Return True
    End Function

    Private Function WriteIntExport(buf() As Byte, name As String, values() As Integer) As Boolean
        Dim room As Integer
        Dim at As Integer = ExportSpan(buf, name, room)
        If at < 0 OrElse room < 4 * values.Length Then Return False
        For k As Integer = 0 To values.Length - 1
            Array.Copy(BitConverter.GetBytes(values(k)), 0, buf, at + 4 * k, 4)
        Next
        Return True
    End Function

    ' The mission DLL the properties describe: a copy of the stub with each value written at its export. Throws
    ' with the reason when it can't be built.
    Public Function BuildDll(stub() As Byte, p As MissionProps) As Byte()
        Dim why As String = Problem(p)
        If why IsNot Nothing Then Throw New InvalidDataException(why)
        Dim b() As Byte = DirectCast(stub.Clone(), Byte())
        Dim ok As Boolean = WriteTextExport(b, "LevelDesc", p.Name) AndAlso
                            WriteTextExport(b, "MapName", p.MapFile) AndAlso
                            WriteTextExport(b, "TechtreeName", p.Tech) AndAlso
                            WriteIntExport(b, "DescBlock", New Integer() {PlacementText.MissionTypeValue(p.TypeName), p.Players, p.MaxTech, If(p.UnitOnly, 1, 0)}) AndAlso
                            WriteIntExport(b, "DescBlockEx", New Integer() {p.AiPlayers})
        If Not ok Then Throw New InvalidDataException("The bundled stub DLL lacks one of the mission exports - it may be the wrong DLL.")
        PatchWideField(b, StubDescPlaceholder, p.Name)   ' FileDescription (Properties > Details), best effort
        Return b
    End Function

    ' ===== the DLL beside a placement =====

    Public Function PlacementBase(placement As String) As String
        Dim f As String = Path.GetFileName(placement)
        If f.ToLowerInvariant().EndsWith(".placement.lua") Then f = f.Substring(0, f.Length - ".placement.lua".Length)
        Return f
    End Function

    ' The mission DLL beside a placement file: c<Base>.dll, else <Base>.dll, else Nothing.
    Public Function ExistingDll(placement As String) As String
        Dim dir As String = Path.GetDirectoryName(placement)
        Dim base As String = PlacementBase(placement)
        For Each c As String In New String() {Path.Combine(dir, "c" & base & ".dll"), Path.Combine(dir, base & ".dll")}
            If File.Exists(c) Then Return c
        Next
        Return Nothing
    End Function

    ' The DLL a placement gets when it has none: c<Base>.dll for a single-player mission whose base starts with a
    ' capital (Outpost 2 lists it under Colony Games and OP2LuaCore drops the c), else <Base>.dll.
    Public Function NewDllPath(placement As String, p As MissionProps) As String
        Dim base As String = PlacementBase(placement)
        Dim capital As Boolean = base.Length > 0 AndAlso base(0) >= "A"c AndAlso base(0) <= "Z"c
        Dim name As String = If(capital AndAlso Not PlacementText.IsMultiplayerType(p.TypeName), "c" & base, base)
        Return Path.Combine(Path.GetDirectoryName(placement), name & ".dll")
    End Function

    ' Whether a file is an OP2Lua stub (it imports OP2LuaCore.dll), which the editor may replace.
    Public Function IsOp2LuaStub(buf() As Byte) As Boolean
        Return Encoding.ASCII.GetString(buf).IndexOf("OP2LuaCore.dll", StringComparison.OrdinalIgnoreCase) >= 0
    End Function

    ' The DLL's values beside a placement (Nothing when there is no readable one).
    Public Function ReadDllBeside(placement As String, ByRef hasAiPlayers As Boolean) As MissionProps
        hasAiPlayers = False
        Dim path_ As String = ExistingDll(placement)
        If path_ Is Nothing Then Return Nothing
        Try
            Return ReadDll(File.ReadAllBytes(path_), hasAiPlayers)
        Catch ex As IOException
            Return Nothing
        End Try
    End Function

    ' What Save would change in the mission's DLL, for a note: its values that differ from the placement text's, or
    ' that there is no DLL yet. Nothing when the DLL already matches.
    Public Function DllNote(placement As String, text As String) As String
        Dim path_ As String = ExistingDll(placement)
        If path_ Is Nothing Then Return "There is no mission DLL beside it yet: Save Mission makes one."
        Dim hasAi As Boolean
        Dim d As MissionProps = ReadDllBeside(placement, hasAi)
        If d Is Nothing Then Return Path.GetFileName(path_) & " can't be read as a mission DLL."
        Dim changes As List(Of String) = Differences(ReadProps(text, d), d, hasAi)
        If changes.Count = 0 Then Return Nothing
        Return Path.GetFileName(path_) & " has " & String.Join(", ", changes) & ": Save writes the placement's values into it."
    End Function

    ' Writes the mission DLL for a placement's text: the DLL beside it (or a new one), built from the stub with the
    ' properties the text declares (the old DLL's for any it doesn't). The previous DLL is kept as .bak; a DLL that
    ' isn't an OP2Lua stub is left alone. Returns what was done; throws with the reason when nothing could be written.
    Public Function SaveDll(placement As String, text As String, stubPath As String) As String
        If String.IsNullOrEmpty(stubPath) OrElse Not File.Exists(stubPath) Then
            Throw New FileNotFoundException("The bundled LuaMission.dll stub isn't beside the editor (a 'stub' folder next to the exe).")
        End If
        Dim existing As String = ExistingDll(placement)
        Dim old() As Byte = If(existing IsNot Nothing, File.ReadAllBytes(existing), Nothing)
        Dim hasAi As Boolean
        Dim oldProps As MissionProps = If(old IsNot Nothing, ReadDll(old, hasAi), Nothing)
        Dim p As MissionProps = ReadProps(text, oldProps)
        If String.IsNullOrWhiteSpace(p.Name) Then p.Name = PlacementBase(placement)
        Dim target As String = If(existing, NewDllPath(placement, p))
        Dim file_ As String = Path.GetFileName(target)
        Dim built() As Byte
        Try
            built = BuildDll(File.ReadAllBytes(stubPath), p)
        Catch ex As InvalidDataException
            Throw New InvalidDataException(file_ & " not written: " & ex.Message)
        End Try
        If old Is Nothing Then
            File.WriteAllBytes(target, built)
            Return "Made " & file_ & "."
        End If
        If old.Length = built.Length AndAlso StructuralComparisons.StructuralEqualityComparer.Equals(old, built) Then
            Return file_ & " was up to date."
        End If
        If Not IsOp2LuaStub(old) Then
            Throw New InvalidDataException(file_ & " isn't an OP2Lua mission DLL, so it was left as it was.")
        End If
        Dim changes As List(Of String) = If(oldProps IsNot Nothing, Differences(p, oldProps, hasAi), New List(Of String)())
        File.Copy(target, target & ".bak", True)
        Try
            File.WriteAllBytes(target, built)
        Catch ex As IOException
            Throw New IOException("Can't write " & file_ & " (is the game running?): " & ex.Message, ex)
        End Try
        If changes.Count = 0 Then Return "Rebuilt " & file_ & " from the editor's stub."
        Return "Rebuilt " & file_ & "; it had " & String.Join(", ", changes) & "."
    End Function

    ' ===== FileDescription =====

    ' Overwrite a UTF-16 string in the DLL's version resource (FileDescription) within its existing room: oldStr plus
    ' the run of NUL words after it, one kept for the terminator. False when oldStr isn't there or newStr won't fit.
    Public Function PatchWideField(buf() As Byte, oldStr As String, newStr As String) As Boolean
        If String.IsNullOrEmpty(oldStr) Then Return False
        Dim needle() As Byte = Encoding.Unicode.GetBytes(oldStr)
        Dim idx As Integer = -1
        Dim from As Integer = 0
        Do
            Dim hit As Integer = IndexOfBytes(buf, needle, from)
            If hit < 0 Then Exit Do
            Dim term As Integer = hit + needle.Length
            If term + 1 < buf.Length AndAlso buf(term) = 0 AndAlso buf(term + 1) = 0 Then idx = hit : Exit Do
            from = hit + 1
        Loop
        If idx < 0 Then Return False
        Dim capWords As Integer = oldStr.Length
        Dim p As Integer = idx + needle.Length
        While p + 1 < buf.Length AndAlso buf(p) = 0 AndAlso buf(p + 1) = 0
            capWords += 1
            p += 2
        End While
        Dim newBytes() As Byte = Encoding.Unicode.GetBytes(newStr)
        If newBytes.Length \ 2 > capWords - 1 Then Return False
        Array.Clear(buf, idx, capWords * 2)
        Array.Copy(newBytes, 0, buf, idx, newBytes.Length)
        Return True
    End Function

    Private Function IndexOfBytes(hay() As Byte, needle() As Byte, from As Integer) As Integer
        If needle.Length = 0 Then Return -1
        For i As Integer = Math.Max(from, 0) To hay.Length - needle.Length
            Dim ok As Boolean = True
            For j As Integer = 0 To needle.Length - 1
                If hay(i + j) <> needle(j) Then ok = False : Exit For
            Next
            If ok Then Return i
        Next
        Return -1
    End Function

End Module
