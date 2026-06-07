Imports System.IO

''' <summary>
''' Settings form for OP2LuaEditor
''' </summary>
Public Class fSettings

    ''' <summary>
    ''' Handles form load - initializes settings form
    ''' </summary>
    Private Sub fSettings_Load(sender As Object, e As EventArgs) Handles MyBase.Load
        ' Set form appearance
        Me.Icon = My.Resources.AppIcon
        Me.Text = ApplicationName & " - Settings"
        lblBuild.Text = "Version " & GlobalVariables.ApplicationVersion

        panelBanner.BackgroundImage = My.Resources.banner02_620x100

        ' Load the current values from My.Settings into the text boxes
        txtWorkingPath.Text = My.Settings.WorkingPath
        txtOP2Path.Text = My.Settings.OP2Path
        txtExportPath.Text = My.Settings.ExportPath
    End Sub

    ''' <summary>
    ''' Handles the click event for the Browse Working Path button
    ''' </summary>
    Private Sub btnBrowseWorkingPath_Click(sender As Object, e As EventArgs) Handles btnBrowseWorkingPath.Click
        Using folderDialog As New FolderBrowserDialog()
            folderDialog.Description = "Select Working Directory for Maps"
            folderDialog.ShowNewFolderButton = True

            ' Set initial directory with proper validation
            If Not String.IsNullOrWhiteSpace(txtWorkingPath.Text) AndAlso Directory.Exists(txtWorkingPath.Text) Then
                folderDialog.SelectedPath = txtWorkingPath.Text
            Else
                ' Default to Documents folder
                folderDialog.SelectedPath = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)
            End If

            If folderDialog.ShowDialog() = DialogResult.OK Then
                txtWorkingPath.Text = folderDialog.SelectedPath
            End If
        End Using
    End Sub

    ''' <summary>
    ''' Handles the click event for the Browse Outpost 2 Path button
    ''' </summary>
    Private Sub btnBrowseOP2Path_Click(sender As Object, e As EventArgs) Handles btnBrowseOP2Path.Click
        Using folderDialog As New FolderBrowserDialog()
            folderDialog.Description = "Select Outpost 2 Game Directory"
            folderDialog.ShowNewFolderButton = False

            ' Set initial directory with proper validation
            If Not String.IsNullOrWhiteSpace(txtOP2Path.Text) AndAlso Directory.Exists(txtOP2Path.Text) Then
                folderDialog.SelectedPath = txtOP2Path.Text
            Else
                ' Default to Program Files
                folderDialog.SelectedPath = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles)
            End If

            If folderDialog.ShowDialog() = DialogResult.OK Then
                Dim selectedPath As String = folderDialog.SelectedPath

                ' Validate the Outpost 2 directory
                If Not File.Exists(Path.Combine(selectedPath, "outpost2.exe")) Then
                    If MessageBox.Show("Could not find outpost2.exe in the selected folder. This may not be a valid Outpost 2 installation. Continue anyway?", "Warning", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) = DialogResult.No Then
                        Return
                    End If
                End If

                txtOP2Path.Text = selectedPath
            End If
        End Using
    End Sub

    ''' <summary>
    ''' Handles the click event for the Browse Export Path button
    ''' </summary>
    Private Sub btnBrowseExportPath_Click(sender As Object, e As EventArgs) Handles btnBrowseExportPath.Click
        Using folderDialog As New FolderBrowserDialog()
            folderDialog.Description = "Select Map Exports Directory"
            folderDialog.ShowNewFolderButton = True

            ' Set initial directory with proper validation
            If Not String.IsNullOrWhiteSpace(txtExportPath.Text) AndAlso Directory.Exists(txtExportPath.Text) Then
                folderDialog.SelectedPath = txtExportPath.Text
            Else
                ' Default to Documents folder
                folderDialog.SelectedPath = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)
            End If

            If folderDialog.ShowDialog() = DialogResult.OK Then
                txtExportPath.Text = folderDialog.SelectedPath
            End If
        End Using
    End Sub

    ''' <summary>
    ''' Handles the click event for the Save button
    ''' </summary>
    Private Sub btnSave_Click(sender As Object, e As EventArgs) Handles btnSave.Click
        ' Validate the Outpost 2 path
        Dim op2Path As String = txtOP2Path.Text.Trim()
        If Not String.IsNullOrEmpty(op2Path) Then
            If Not Directory.Exists(op2Path) Then
                MessageBox.Show("The specified Outpost 2 directory does not exist.", "Invalid Path", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End If
        End If

        ' Create directories if they don't exist (for working and export paths)
        Dim workingPath As String = txtWorkingPath.Text.Trim()
        If Not String.IsNullOrEmpty(workingPath) AndAlso Not Directory.Exists(workingPath) Then
            Try
                Directory.CreateDirectory(workingPath)
            Catch ex As Exception
                MessageBox.Show("Could not create working directory: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try
        End If

        Dim exportPath As String = txtExportPath.Text.Trim()
        If Not String.IsNullOrEmpty(exportPath) AndAlso Not Directory.Exists(exportPath) Then
            Try
                Directory.CreateDirectory(exportPath)
            Catch ex As Exception
                MessageBox.Show("Could not create export directory: " & ex.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error)
                Return
            End Try
        End If

        ' Save all settings
        My.Settings.WorkingPath = workingPath
        My.Settings.OP2Path = op2Path
        My.Settings.ExportPath = exportPath
        My.Settings.Save()

        ' Close the form
        Me.DialogResult = DialogResult.OK
        Me.Close()

        'fMain.InitializeMapResourceManager()

        fMain.WindowState = FormWindowState.Normal
        fMain.BringToFront()
        fMain.Activate()
        fMain.Show()
    End Sub

    ''' <summary>
    ''' Handles the click event for the Cancel button
    ''' </summary>
    Private Sub btnCancel_Click(sender As Object, e As EventArgs) Handles btnCancel.Click
        ' Just close the form without saving
        Me.DialogResult = DialogResult.Cancel
        Me.Close()
    End Sub
End Class