<?php
// Plesk Obsidian MVC controller — Extensions paneli UI'yi buradan dispatch eder
// (/usr/local/psa/admin/plib/modules/look-lang/controllers/IndexController.php).
// Kendi kendine yeten htdocs/index.php'ye köprü kurar: auto_setup + AJAX (?action)
// + sayfa render (phtml). Böylece tek mantık yeri korunur.
class IndexController extends pm_Controller_Action
{
    public function indexAction()
    {
        // htdocs/index.php doğrudan HTML/JSON basar → Zend layout/view'i kapat.
        $this->_helper->layout->disableLayout();
        $this->_helper->viewRenderer->setNoRender(true);

        $htdocs = '/usr/local/psa/admin/htdocs/modules/look-lang/index.php';
        if (is_file($htdocs)) {
            include $htdocs;   // auto_setup + (?action ? JSON+exit : phtml render)
        } else {
            echo 'LOOK Language: htdocs/index.php bulunamadı.';
        }
    }
}
